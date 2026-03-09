#include "uac_microphone.h"
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
static TaskHandle_t             s_ptt_task      = NULL;

/* ── UAC device event callback ───────────────────────────────── */

static void uac_device_event_cb(uac_host_device_handle_t dev,
                                const uac_host_device_event_t event,
                                void *arg)
{
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        s_uac_connected = false;
        s_mic_handle    = NULL;
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

    /* Try to detect preferred sample rate from device alt params */
    uac_host_dev_alt_param_t alt;
    for (uint8_t i = 1; i <= 8; i++) {
        if (uac_host_get_device_alt_param(s_mic_handle, i, &alt) != ESP_OK) break;
        if (alt.channels > 0 && alt.sample_freq_type > 0) {
            uint32_t freq = alt.sample_freq[0];
            /* Prefer 16 kHz (Whisper native); accept anything */
            if (freq == 16000 || s_sample_rate != 16000) {
                s_sample_rate = freq;
                if (freq == 16000) break;
            }
        }
    }

    s_uac_connected = true;
    ESP_LOGI(TAG, "UAC mic ready: preferred rate %" PRIu32 " Hz", s_sample_rate);
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

    while (1) {
        bool pressed = s_uac_connected && (gpio_get_level(LANG_PTT_GPIO) == 0);

        /* Periodic diagnostic: log UAC connection state every 5 s while idle */
        if (!was_pressed) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_log_tick) > pdMS_TO_TICKS(5000)) {
                ESP_LOGI(TAG, "UAC PTT idle: connected=%d", s_uac_connected);
                last_log_tick = now;
            }
        }

        if (pressed && !was_pressed) {
            /* ── Button just pressed — open stream + WAV ring ── */
            /* Try formats in preference order: 16kHz → 44.1kHz → 48kHz, mono first */
            static const uac_host_stream_config_t fmts[] = {
                { .channels = 1, .bit_resolution = 16, .sample_freq = 16000 },
                { .channels = 1, .bit_resolution = 16, .sample_freq = 44100 },
                { .channels = 1, .bit_resolution = 16, .sample_freq = 48000 },
                { .channels = 2, .bit_resolution = 16, .sample_freq = 44100 },
                { .channels = 2, .bit_resolution = 16, .sample_freq = 48000 },
            };
            esp_err_t sret = ESP_FAIL;
            uint32_t  rate = 16000;
            for (size_t i = 0; i < sizeof(fmts)/sizeof(fmts[0]); i++) {
                sret = uac_host_device_start(s_mic_handle, &fmts[i]);
                if (sret == ESP_OK) {
                    rate = fmts[i].sample_freq;
                    ESP_LOGI(TAG, "UAC stream started: %" PRIu32 " Hz/%d-bit/%dch",
                             fmts[i].sample_freq, fmts[i].bit_resolution, fmts[i].channels);
                    break;
                }
            }
            if (sret != ESP_OK) {
                ESP_LOGE(TAG, "uac_host_device_start: no supported format, skipping PTT");
                while (gpio_get_level(LANG_PTT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            esp_err_t rret = audio_ring_open_wav(PTT_CHAT_ID, rate, 1, 16);
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
                ret = audio_ring_append(chunk, (size_t)got);
                if (ret == ESP_ERR_NO_MEM) {
                    ESP_LOGW(TAG, "Audio ring overflow — dropping recording");
                    uac_host_device_stop(s_mic_handle);
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

    /* Increased delay (2 s): the USB host may not replay DEVICE_CONNECTED to class
     * drivers that register after enumeration completes.  If the webcam finishes
     * UVC enumeration in < 500 ms, uac_driver_event_cb never fires → s_uac_connected
     * stays false.  2 s gives the USB stack ample time to re-announce the device
     * to any newly-registered class driver on most hardware. */
    vTaskDelay(pdMS_TO_TICKS(2000));

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
