#include "uac_microphone.h"
#include "audio/downsample.h"
#include "audio/audio_pipeline.h"
#include "led/led_indicator.h"
#include "langoustine_config.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "usb/uac_host.h"

static const char *TAG = "uac_mic";

#define PTT_CHAT_ID  "ptt"
#define PTT_MAX_MS   8000

/* Internal UAC buffer: ~500 ms of 16 kHz/16-bit/mono PCM */
#define UAC_BUF_SIZE       (16000)
#define UAC_BUF_THRESHOLD  (512)   /* trigger RX_DONE every 512 bytes */

static uac_host_device_handle_t s_mic_handle   = NULL;
static volatile bool            s_uac_connected = false;
static bool                     s_initialized   = false;
static uint32_t                 s_sample_rate   = 16000;
static uint8_t                  s_channels      = 1;
static TaskHandle_t             s_ptt_task      = NULL;
/* Set when uac_host_device_start() succeeds; cleared when stop is called.
 * Prevents double-stop of a stream that never started — double-stop corrupts
 * the UAC internal transfer heap and causes StoreProhibited crashes in the
 * FreeRTOS context switch path (vPortYieldFromInt). */
static volatile bool            s_stream_running = false;
/* Set when all format attempts fail; cleared only on disconnect+reconnect.
 * Prevents repeated crash loops when the USB device is in a bad state. */
static volatile bool            s_start_failed  = false;

/* ── UAC device event callback ───────────────────────────────── */

static void uac_device_event_cb(uac_host_device_handle_t dev,
                                const uac_host_device_event_t event,
                                void *arg)
{
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        s_uac_connected  = false;
        s_mic_handle     = NULL;
        s_stream_running = false;
        s_start_failed   = false;   /* allow retry after replug */
        ESP_LOGW(TAG, "UAC mic disconnected");
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "UAC transfer error");
        break;
    default:
        break;
    }
}

/* ── UAC driver event callback ───────────────────────────────── */

static void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event,
                                void *arg)
{
    if (event != UAC_HOST_DRIVER_EVENT_RX_CONNECTED) return;

    ESP_LOGI(TAG, "UAC device connected (addr=%d, iface=%d)", addr, iface_num);

    uac_host_device_config_t dev_cfg = {
        .addr             = addr,
        .iface_num        = iface_num,
        .buffer_size      = UAC_BUF_SIZE,
        .buffer_threshold = UAC_BUF_THRESHOLD,
        .callback         = uac_device_event_cb,
        .callback_arg     = NULL,
    };
    esp_err_t err = uac_host_device_open(&dev_cfg, &s_mic_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    /* Log available formats */
    uac_host_printf_device_param(s_mic_handle);

    /* Detect supported sample rate + channels from device alt params.
     * Prefer 16 kHz mono (Whisper native); fall back to whatever the device
     * actually supports — typically 44100 or 48000 Hz mono/stereo. */
    uac_host_dev_alt_param_t alt;
    bool found = false;
    for (uint8_t i = 1; i <= 8; i++) {
        if (uac_host_get_device_alt_param(s_mic_handle, i, &alt) != ESP_OK) break;
        if (alt.channels > 0 && alt.sample_freq_type > 0) {
            uint32_t freq = alt.sample_freq[0];
            ESP_LOGI(TAG, "  alt %d: %"PRIu32" Hz, %d ch, %d bit",
                     i, freq, alt.channels, alt.bit_resolution);
            if (freq == 16000 && alt.channels == 1) {
                /* Perfect match — use it */
                s_sample_rate = 16000;
                s_channels    = 1;
                found = true;
                break;
            }
            if (!found || s_sample_rate == 16000) {
                /* Accept first available, or replace a 16kHz guess */
                s_sample_rate = freq;
                s_channels    = alt.channels;
                found = true;
            }
        }
    }

    s_uac_connected = true;
    ESP_LOGI(TAG, "UAC mic ready: %"PRIu32" Hz / %d ch", s_sample_rate, s_channels);
}

/* ── PTT task (Core 0) ───────────────────────────────────────── */

static void uac_ptt_task(void *arg)
{
    ESP_LOGI(TAG, "UAC PTT task started on Core %d (GPIO %d)",
             xPortGetCoreID(), LANG_PTT_GPIO);

    bool       was_pressed   = false;
    TickType_t press_start   = 0;
    TickType_t last_log_tick = 0;
    uint8_t    chunk[512];
    int        ds_ratio      = 1;  /* downsampling ratio: 1=none, 3=48kHz→16kHz */

    while (1) {
        bool pressed = s_uac_connected && !s_start_failed && (gpio_get_level(LANG_PTT_GPIO) == 0);

        /* Periodic diagnostic: log UAC connection state every 30 s while idle.
         * Kept at DEBUG in normal builds; demoted from INFO to avoid flooding
         * the 16 KB log ring buffer with noise that buries real events. */
        if (!was_pressed) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_log_tick) > pdMS_TO_TICKS(30000)) {
                ESP_LOGD(TAG, "UAC PTT idle: connected=%d", s_uac_connected);
                last_log_tick = now;
            }
        }

        if (pressed && !was_pressed) {
            /* ── Button just pressed — open stream + WAV ring ── */
            /* Only stop a stream that was actually started — calling stop on
             * an un-started or already-stopped handle corrupts UAC internal
             * transfer state and causes a StoreProhibited crash in the
             * FreeRTOS context switch when the USB interrupt next fires. */
            if (s_stream_running) {
                uac_host_device_stop(s_mic_handle);
                s_stream_running = false;
            }

            /* Single format attempt using rate/channels detected at connect time.
             * Do NOT retry other formats. Each failed uac_host_device_start() leaks
             * a UAC transfer descriptor ("Unable to release UAC Interface"), and
             * even 3-5 leaked descriptors corrupt the TLSF heap, causing
             * LoadProhibited crashes when the heap is next walked (e.g. during
             * heartbeat LittleFS access). One attempt, one leak max. */
            uac_host_stream_config_t detected_fmt = {
                .channels       = s_channels,
                .bit_resolution = 16,
                .sample_freq    = s_sample_rate,
            };
            uint32_t  rate = s_sample_rate;
            esp_err_t sret = uac_host_device_start(s_mic_handle, &detected_fmt);
            if (sret == ESP_OK) {
                s_stream_running = true;
                ESP_LOGI(TAG, "UAC stream started: %"PRIu32" Hz/16-bit/%dch",
                         rate, s_channels);
            } else {
                ESP_LOGE(TAG, "uac_host_device_start failed (%s) — marking broken, replug USB to recover",
                         esp_err_to_name(sret));
                s_start_failed = true;
                led_indicator_set(LED_ERROR);
                while (gpio_get_level(LANG_PTT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            /* Downsample to ~16 kHz if the device negotiated a higher rate.
             * 48 kHz → 16 kHz is exact 3:1; 44.1 kHz → 22050 Hz (2:1).
             * Without downsampling, 48 kHz mono overflows the 256 KB ring
             * after ~2.7 s; 44.1 kHz after ~3 s.  Ratio >= 2 is essential. */
            ds_ratio = (rate >= 32000) ? (int)(rate / 16000) : 1;
            uint32_t wav_rate = rate / (uint32_t)ds_ratio;
            ESP_LOGI(TAG, "UAC PTT: raw %"PRIu32" Hz, ds_ratio=%d, WAV rate %"PRIu32" Hz",
                     rate, ds_ratio, wav_rate);

            esp_err_t rret = audio_ring_open_wav(PTT_CHAT_ID, wav_rate, 1, 16,
                                                  LANG_CHAN_WEBSOCKET);
            if (rret != ESP_OK) {
                ESP_LOGW(TAG, "audio_ring_open_wav failed: %s", esp_err_to_name(rret));
                uac_host_device_stop(s_mic_handle);
                while (gpio_get_level(LANG_PTT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            led_indicator_set(LED_LISTENING);
            press_start = xTaskGetTickCount();
            was_pressed = true;

        } else if (!pressed && was_pressed) {
            /* ── Button released — stop stream + commit ── */
            uac_host_device_stop(s_mic_handle);
            s_stream_running = false;
            audio_ring_patch_wav_sizes();
            audio_ring_commit(PTT_CHAT_ID);
            led_indicator_set(LED_THINKING);
            was_pressed = false;

        } else if (pressed && was_pressed) {
            /* ── Button held — drain audio from internal UAC buffer ── */
            uint32_t got = 0;
            esp_err_t ret = uac_host_device_read(s_mic_handle, chunk, sizeof(chunk),
                                                  &got, pdMS_TO_TICKS(20));
            if (ret == ESP_OK && got > 0) {
                /* Stereo → mono if needed, then rate downsample */
                if (s_channels == 2) {
                    got = (uint32_t)pcm16_stereo_to_mono(chunk, (size_t)got);
                }
                got = (uint32_t)pcm16_downsample(chunk, (size_t)got, ds_ratio);
                ret = audio_ring_append(chunk, (size_t)got);
                if (ret == ESP_ERR_NO_MEM) {
                    ESP_LOGW(TAG, "Audio ring overflow — dropping recording");
                    uac_host_device_stop(s_mic_handle);
                    s_stream_running = false;
                    led_indicator_set(LED_ERROR);
                    was_pressed = false;
                    while (gpio_get_level(LANG_PTT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            }

            /* Auto-commit after max duration */
            if ((xTaskGetTickCount() - press_start) > pdMS_TO_TICKS(PTT_MAX_MS)) {
                ESP_LOGW(TAG, "UAC PTT max duration (%d s) — auto-committing",
                         PTT_MAX_MS / 1000);
                uac_host_device_stop(s_mic_handle);
                s_stream_running = false;
                audio_ring_patch_wav_sizes();
                audio_ring_commit(PTT_CHAT_ID);
                led_indicator_set(LED_THINKING);
                was_pressed = false;
                while (gpio_get_level(LANG_PTT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;   /* tight loop while held */
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t uac_microphone_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Configure BOOT button (GPIO0) as input with pull-up */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LANG_PTT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* No delay here.  uvc_camera_start_host_task() is called AFTER this function
     * returns, so the USB host event task hasn't started yet when UAC registers.
     * Both UVC and UAC drivers are therefore registered before the USB daemon
     * fires its first device-connected event — no replay needed. */

    /* Install UAC driver — USB host must already be running (uvc_camera_init done) */
    uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority          = 4,   /* one below UVC (5) to avoid priority inversion */
        .stack_size             = 6144,
        .core_id                = 0,
        .callback               = uac_driver_event_cb,
        .callback_arg           = NULL,
    };
    err = uac_host_install(&uac_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UAC mic driver installed — waiting for device on GPIO %d PTT",
             LANG_PTT_GPIO);
    return ESP_OK;
}

void uac_microphone_start(void)
{
    if (s_ptt_task) return;
    BaseType_t ret = xTaskCreatePinnedToCore(
        uac_ptt_task, "uac_ptt",
        LANG_MIC_STACK_SIZE, NULL,
        LANG_MIC_TASK_PRIO,
        &s_ptt_task,
        0);   /* Core 0 — keeps Core 1 free for agent */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UAC PTT task");
        s_ptt_task = NULL;
    }
}

bool uac_microphone_available(void)
{
    return s_initialized && s_uac_connected;
}

void uac_microphone_deinit(void)
{
    if (!s_initialized) return;
    if (s_mic_handle) {
        uac_host_device_stop(s_mic_handle);
        uac_host_device_close(s_mic_handle);
        s_mic_handle = NULL;
    }
    uac_host_uninstall();
    if (s_ptt_task) {
        vTaskDelete(s_ptt_task);
        s_ptt_task = NULL;
    }
    s_initialized   = false;
    s_uac_connected = false;
}
