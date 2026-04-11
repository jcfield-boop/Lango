/*
 * Wake Word Detection — ESP-SR AFE + WakeNet9 ("Hi ESP")
 *
 * Uses the ESP-SR v2.x Audio Front End (AFE) on ESP32-S3.
 * Two tasks cooperate:
 *   feed_task  — reads raw PCM from I2S RX, feeds into AFE (Core 0)
 *   detect_task — fetches processed results from AFE, handles wake word,
 *                 PTT GPIO, VAD endpoint, and recording (Core 0)
 *
 * If the esp-sr component is not present this file compiles to stubs
 * that return ESP_ERR_NOT_SUPPORTED so langoustine.c falls back to PTT only.
 */
#include "wake_word.h"
#include "audio/i2s_audio.h"
#include "audio/audio_pipeline.h"
#include "led/led_indicator.h"
#include "gateway/ws_server.h"
#include "langoustine_config.h"
#include "memory/psram_alloc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <inttypes.h>
#include <math.h>

static const char *TAG = "wake_word";

/* ── Conditional compilation: only build if esp-sr is available ─── */

#if __has_include("esp_afe_sr_iface.h")

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"  /* esp_afe_handle_from_config() */
/* esp_srmodel_init() + srmodel_list_t come from model_path.h via esp_afe_config.h */

/* ── Tuning constants ─────────────────────────────────────────── */

/* chat_id for all wake-word/PTT local recordings */
#define WW_CHAT_ID "ptt"

/* Max ms between wake word detection and first speech before aborting */
#define WW_SPEECH_TIMEOUT_MS   10000

/* ms of VAD silence after speech to trigger end-of-utterance.
 * 700ms feels natural — short enough to be responsive, long enough not to
 * cut off mid-sentence.  1500ms was too conservative for voice assistant use. */
#define WW_VAD_SILENCE_MS        500

/* Max recording duration (safety limit) */
#define WW_MAX_RECORD_MS        8000

/* Default tuning values (can be overridden via NVS) */
#define WW_DEFAULT_GAIN       4.0f    /* 10.0 clipped signal; 3.0 too low; 4.0 balanced */
#define WW_DEFAULT_THRESHOLD  0.60f   /* 0.50 fired on radio voices, 0.70 missed the user — 0.60 is the sweet spot */
#define WW_AFE_GAIN           1.0f   /* AFE's own gain — keep at 1.0, we apply software gain */

/* NVS namespace and keys for persistent wake word config */
#define WW_NVS_NAMESPACE "ww_config"
#define WW_NVS_KEY_GAIN  "gain"
#define WW_NVS_KEY_THRESH "threshold"

/* ── Module state ─────────────────────────────────────────────── */

static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static esp_afe_sr_data_t        *s_afe_data  = NULL;
static int                       s_feed_chunk = 0;   /* samples per feed() call */

/* Shared between feed_task and detect_task (detect_task reads PTT too) */
static TaskHandle_t s_feed_task_handle   = NULL;
static TaskHandle_t s_detect_task_handle = NULL;

/* Cooperative suspend: tasks check this flag and voluntarily pause.
 * Using vTaskSuspend is unsafe because it can freeze a task mid-read
 * while holding the I2S DMA semaphore → deadlock on i2s_channel_disable. */
static volatile bool s_suspended = false;

/* Runtime-adjustable parameters (atomic float via volatile + single-writer) */
static volatile float s_sw_gain     = WW_DEFAULT_GAIN;      /* software pre-gain */
static volatile float s_wn_threshold = WW_DEFAULT_THRESHOLD; /* WakeNet threshold */

/* Test mode: when > 0, detect_task prints every speech/wakeup event.
 * Decremented each second; auto-expires to 0. */
static volatile int s_test_seconds = 0;

/* Cumulative counters for snapshot-based testing */
static volatile uint32_t s_total_feeds    = 0;
static volatile uint32_t s_total_fetches  = 0;
static volatile uint32_t s_total_speech   = 0;
static volatile uint32_t s_total_wakeups  = 0;
static volatile float    s_last_volume_db = -99.0f;
static volatile int16_t  s_test_peak_pos  = 0;
static volatile int16_t  s_test_peak_neg  = 0;

/* ── NVS helpers ─────────────────────────────────────────────── */

static void ww_nvs_save_float(const char *key, float val)
{
    nvs_handle_t h;
    if (nvs_open(WW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t bits;
        memcpy(&bits, &val, sizeof(bits));
        nvs_set_u32(h, key, bits);
        nvs_commit(h);
        nvs_close(h);
    }
}

static float ww_nvs_load_float(const char *key, float def)
{
    nvs_handle_t h;
    float result = def;
    if (nvs_open(WW_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t bits;
        if (nvs_get_u32(h, key, &bits) == ESP_OK) {
            memcpy(&result, &bits, sizeof(result));
        }
        nvs_close(h);
    }
    return result;
}

/* ── Feed task (Core 0) ──────────────────────────────────────── */

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "AFE feed task started (chunk=%d samples)", s_feed_chunk);

    /* i2s_audio_read() receives 32-bit DMA words and extracts 16-bit samples
     * in-place.  To get N 16-bit samples we must pass N×4 bytes (32-bit DMA).
     *
     * The slave RX DMA only fills the ring buffer once per enable cycle
     * (8 descriptors × 480 frames × 4B = 15360 bytes, taking ~240ms at 16kHz).
     * Reading the full ring per cycle amortises the 30ms DMA restart delay
     * over ~7 AFE feeds instead of one.
     *
     * Buffer layout: [--- 32-bit DMA data (read_buf_sz bytes) ---]
     * After extraction: [--- 16-bit samples (read_buf_sz/2 bytes) ---] */
    const size_t read_buf_sz = LANG_I2S_DMA_DESC_NUM * LANG_I2S_DMA_FRAME_NUM * 4;
    int16_t *buf = (int16_t *)ps_malloc(read_buf_sz);
    if (!buf) {
        ESP_LOGE(TAG, "feed buf alloc failed (%u bytes)", (unsigned)read_buf_sz);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Feed buffer: %u bytes (holds %u 16-bit samples after extraction, "
             "%d AFE feeds per DMA cycle)",
             (unsigned)read_buf_sz, (unsigned)(read_buf_sz / 4),
             (int)(read_buf_sz / 4 / s_feed_chunk));

    uint32_t feed_count = 0;
    uint32_t fail_count = 0;
    int16_t  peak_val   = 0;
    int16_t  min_val    = 0;
    int32_t  sum_val    = 0;
    uint32_t sum_count  = 0;
    bool     first_diag = true;
    TickType_t last_diag = xTaskGetTickCount();

    while (1) {
        /* Cooperative suspend: yield while paused (releases all locks) */
        while (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* Read a large chunk of 32-bit DMA data.  i2s_audio_read() does
         * disable+enable+30ms delay once, then reads up to read_buf_sz
         * bytes with 500ms timeout (enough for all 8 DMA descriptors).
         * Returns 16-bit extracted samples in buf. */
        size_t got = 0;
        esp_err_t ret = i2s_audio_read((uint8_t *)buf, read_buf_sz,
                                       &got, 500);
        if (ret != ESP_OK || got == 0) {
            fail_count++;
            vTaskDelay(1);  /* yield to IDLE on failure path */
            continue;
        }

        /* got = bytes of 16-bit samples after extraction.
         * Feed AFE in s_feed_chunk-sized pieces. */
        size_t total_samples = got / sizeof(int16_t);
        size_t offset = 0;

        while (offset + (size_t)s_feed_chunk <= total_samples) {
            int16_t *chunk = buf + offset;

            /* Apply software pre-gain (fixed-point Q16) */
            float cur_gain = s_sw_gain;
            if (cur_gain != 1.0f) {
                int32_t gain_q16 = (int32_t)(cur_gain * 65536.0f);
                for (int i = 0; i < s_feed_chunk; i++) {
                    int32_t v = ((int32_t)chunk[i] * gain_q16) >> 16;
                    if (v > 32767)  v = 32767;
                    if (v < -32768) v = -32768;
                    chunk[i] = (int16_t)v;
                }
            }

            s_afe_iface->feed(s_afe_data, chunk);
            feed_count++;
            s_total_feeds++;

            /* Track peak, min, sum for diagnostics */
            for (int i = 0; i < s_feed_chunk; i++) {
                int16_t v = chunk[i];
                if (v > peak_val) peak_val = v;
                if (v < min_val)  min_val  = v;
                if (v > s_test_peak_pos) s_test_peak_pos = v;
                if (v < s_test_peak_neg) s_test_peak_neg = v;
                sum_val += v;
                sum_count++;
            }

            offset += s_feed_chunk;
        }

        /* Yield to IDLE task after each DMA read+feed cycle.
         * Without this, the feed loop (read 300ms + feed 7 chunks)
         * starves IDLE0 and triggers the task watchdog. */
        vTaskDelay(1);

        /* Periodic diagnostic every 30s — visible at INFO level so we can
         * confirm the AFE pipeline is alive without enabling verbose logs. */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_diag) > pdMS_TO_TICKS(30000)) {
            int32_t dc_offset = sum_count ? (sum_val / (int32_t)sum_count) : 0;
            ESP_LOGI(TAG, "Feed: %"PRIu32" ok, %"PRIu32" fail, peak=[%d,%d] dc=%"PRId32" gain=%.1f",
                     feed_count, fail_count,
                     (int)min_val, (int)peak_val, dc_offset,
                     (double)s_sw_gain);

            if (first_diag && total_samples >= 16) {
                ESP_LOGI(TAG, "Raw samples[0..15]: %d %d %d %d %d %d %d %d "
                         "%d %d %d %d %d %d %d %d",
                         buf[0], buf[1], buf[2], buf[3],
                         buf[4], buf[5], buf[6], buf[7],
                         buf[8], buf[9], buf[10], buf[11],
                         buf[12], buf[13], buf[14], buf[15]);
                first_diag = false;
            }

            char diag[140];
            snprintf(diag, sizeof(diag),
                     "feeds=%"PRIu32" fails=%"PRIu32" min=%d max=%d dc=%"PRId32
                     " gain=%.1f",
                     feed_count, fail_count,
                     (int)min_val, (int)peak_val, dc_offset,
                     (double)s_sw_gain);
            feed_count = 0;
            fail_count = 0;
            peak_val   = 0;
            min_val    = 0;
            sum_val    = 0;
            sum_count  = 0;
            last_diag  = now;
        }
    }
}

/* ── Detect task (Core 0) ───────────────────────────────────── */

static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "AFE detect task started");

    int  fetch_chunk    = s_afe_iface->get_fetch_chunksize(s_afe_data);
    bool recording      = false;
    bool ptt_active     = false;
    bool speech_started = false;
    TickType_t activate_tick    = 0;   /* when recording started (wake word) */
    TickType_t silence_start    = 0;   /* when VAD silence began */
    uint32_t   detect_count     = 0;
    uint32_t   speech_frames    = 0;
    TickType_t last_detect_diag = xTaskGetTickCount();

    while (1) {
        /* Cooperative suspend: yield while paused */
        while (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* ── Check PTT button ────────────────────────────────── */
        bool ptt = (gpio_get_level(LANG_PTT_GPIO) == 0);

        if (ptt && !ptt_active && !recording) {
            /* PTT just pressed — start recording immediately */
            if (audio_ring_open_wav(WW_CHAT_ID, LANG_MIC_SAMPLE_RATE, 1,
                                     LANG_MIC_BITS, LANG_CHAN_WEBSOCKET) == ESP_OK) {
                led_indicator_set(LED_LISTENING);
                recording      = true;
                ptt_active     = true;
                speech_started = true;  /* PTT → no wait-for-speech timeout */
                activate_tick  = xTaskGetTickCount();
                silence_start  = 0;
                ESP_LOGI(TAG, "PTT pressed — recording");
                ws_server_broadcast_monitor("wake_word", "PTT pressed — recording");
            }
        } else if (!ptt && ptt_active) {
            /* PTT released — commit */
            ptt_active = false;
            if (recording) {
                ESP_LOGI(TAG, "PTT released — committing");
                audio_ring_patch_wav_sizes();
                audio_ring_commit(WW_CHAT_ID);
                led_indicator_set(LED_THINKING);
                recording      = false;
                speech_started = false;
                silence_start  = 0;
            }
        }

        /* ── Fetch AFE result ────────────────────────────────── */
        afe_fetch_result_t *res = s_afe_iface->fetch(s_afe_data);
        if (!res) continue;

        detect_count++;
        s_total_fetches++;
        if (res->vad_state == VAD_SPEECH) {
            speech_frames++;
            s_total_speech++;
        }
        s_last_volume_db = res->data_volume;
        if (res->wakeup_state == WAKENET_DETECTED) {
            s_total_wakeups++;
        }

        /* Test mode: print detailed per-event info */
        if (s_test_seconds > 0) {
            if (res->vad_state == VAD_SPEECH) {
                ESP_LOGI(TAG, "[TEST] SPEECH vol=%.0fdB wakeup=%d",
                         res->data_volume, (int)res->wakeup_state);
            }
            if (res->wakeup_state != WAKENET_NO_DETECT) {
                ESP_LOGW(TAG, "[TEST] WAKEUP state=%d word_idx=%d model_idx=%d vol=%.0fdB",
                         (int)res->wakeup_state, res->wake_word_index,
                         res->wakenet_model_index, res->data_volume);
            }
        }

        /* Periodic detect diagnostic every 10 s (verbose only) */
        {
            TickType_t now_d = xTaskGetTickCount();
            if ((now_d - last_detect_diag) > pdMS_TO_TICKS(30000)) {
                ESP_LOGI(TAG, "Detect: %"PRIu32" fetches, %"PRIu32" speech, vol=%.0fdB rec=%d",
                         detect_count, speech_frames,
                         res->data_volume, (int)recording);
                char diag[140];
                snprintf(diag, sizeof(diag),
                         "fetches=%"PRIu32" speech=%"PRIu32" rec=%d vol=%.0fdB",
                         detect_count, speech_frames,
                         (int)recording, res->data_volume);
                detect_count     = 0;
                speech_frames    = 0;
                last_detect_diag = now_d;
            }
        }

        /* ── Wake word detection ─────────────────────────────── */
        if (!recording && !ptt_active &&
            res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, "*** WAKE WORD 'Hi ESP' DETECTED ***");
            ws_server_broadcast_monitor("wake_word", "Hi ESP detected");
            if (audio_ring_open_wav(WW_CHAT_ID, LANG_MIC_SAMPLE_RATE, 1,
                                     LANG_MIC_BITS, LANG_CHAN_WEBSOCKET) == ESP_OK) {
                led_indicator_set(LED_LISTENING);
                recording      = true;
                speech_started = false;
                activate_tick  = xTaskGetTickCount();
                silence_start  = 0;
                ESP_LOGI(TAG, "Recording started after wake word");
            }
        }

        /* ── Recording: append AFE-processed audio ───────────── */
        if (recording) {
            if (res->data && res->data_size > 0) {
                audio_ring_append((uint8_t *)res->data,
                                  (size_t)res->data_size);
            }

            TickType_t now = xTaskGetTickCount();

            /* Track VAD for endpoint detection (wake word mode only) */
            if (!ptt_active) {
                if (res->vad_state == VAD_SPEECH) {
                    speech_started = true;
                    silence_start  = 0;
                } else {
                    /* VAD silence */
                    if (speech_started) {
                        if (silence_start == 0) silence_start = now;
                        if (now - silence_start >
                                pdMS_TO_TICKS(WW_VAD_SILENCE_MS)) {
                            ESP_LOGI(TAG, "VAD silence — committing recording");
                            ws_server_broadcast_monitor("wake_word", "VAD silence — committing");
                            audio_ring_patch_wav_sizes();
                            audio_ring_commit(WW_CHAT_ID);
                            led_indicator_set(LED_THINKING);
                            recording      = false;
                            speech_started = false;
                            silence_start  = 0;
                            continue;
                        }
                    } else {
                        /* No speech yet — check for activation timeout */
                        if (now - activate_tick >
                                pdMS_TO_TICKS(WW_SPEECH_TIMEOUT_MS)) {
                            ESP_LOGW(TAG, "Wake word timeout — aborting");
                            audio_ring_reset();
                            led_indicator_set(LED_READY);
                            recording     = false;
                            silence_start = 0;
                            continue;
                        }
                    }
                }
            }

            /* Safety: max recording duration */
            if (now - activate_tick > pdMS_TO_TICKS(WW_MAX_RECORD_MS)) {
                ESP_LOGW(TAG, "Max recording duration — auto-committing");
                audio_ring_patch_wav_sizes();
                audio_ring_commit(WW_CHAT_ID);
                led_indicator_set(LED_THINKING);
                recording      = false;
                ptt_active     = false;
                speech_started = false;
                silence_start  = 0;
            }
        }

        (void)fetch_chunk;  /* used for documentation; data_size is authoritative */
    }
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t wake_word_init(void)
{
    /* Load runtime-tunable params from NVS (or use defaults).
     * User-set values via /api/config or ww_threshold CLI always win — no clamp. */
    s_sw_gain      = ww_nvs_load_float(WW_NVS_KEY_GAIN,  WW_DEFAULT_GAIN);
    s_wn_threshold = ww_nvs_load_float(WW_NVS_KEY_THRESH, WW_DEFAULT_THRESHOLD);
    /* Sanity clamp: gain must be in (0.1, 50.0) — matches the setter's range.
     * No floor at WW_DEFAULT_GAIN — user can tune down below default for mics
     * that are already hot (e.g. railing samples). */
    if (s_sw_gain < 0.1f || s_sw_gain > 50.0f) {
        ESP_LOGW(TAG, "NVS gain %.1f out of range — resetting to %.1f",
                 (double)s_sw_gain, (double)WW_DEFAULT_GAIN);
        s_sw_gain = WW_DEFAULT_GAIN;
        ww_nvs_save_float(WW_NVS_KEY_GAIN, s_sw_gain);
    }
    /* Sanity clamp: threshold must be in (0.0, 1.0) */
    if (s_wn_threshold <= 0.0f || s_wn_threshold >= 1.0f) {
        ESP_LOGW(TAG, "NVS threshold %.3f out of range — resetting to %.3f",
                 (double)s_wn_threshold, (double)WW_DEFAULT_THRESHOLD);
        s_wn_threshold = WW_DEFAULT_THRESHOLD;
        ww_nvs_save_float(WW_NVS_KEY_THRESH, s_wn_threshold);
    }
    ESP_LOGI(TAG, "WW config: sw_gain=%.2f threshold=%.3f",
             (double)s_sw_gain, (double)s_wn_threshold);

    /* Load models from the "model" flash partition (must exist in partitions CSV) */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGW(TAG, "No ESP-SR models in flash — wake word unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    /* Single-microphone AFE configuration.
     * Input format "M" = one microphone channel, no reference.
     * AFE_TYPE_SR = speech recognition (WakeNet + VAD).
     * AFE_MODE_LOW_COST = tuned for single-core ESP32-S3. */
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    /* Force WakeNet on — AFE_MODE_LOW_COST may disable it in some ESP-SR versions */
    if (!cfg->wakenet_init) {
        ESP_LOGW(TAG, "WakeNet was disabled by afe_config_init — forcing on");
        cfg->wakenet_init = true;
    }

    /* Keep AFE's own gain at 1.0 — we apply software gain in feed_task
     * before calling afe->feed().  This way the gain can be adjusted at
     * runtime without reinitializing the AFE. */
    cfg->afe_linear_gain = WW_AFE_GAIN;

    /* Enable AGC on the AFE output (post-WakeNet).
     * This normalizes the audio returned by fetch() for better STT quality.
     * Note: AGC does NOT affect what WakeNet sees — detection still depends
     * on the software pre-gain applied in feed_task before afe->feed(). */
    cfg->agc_init = true;
    cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    cfg->agc_compression_gain_db = 18;  /* aggressive: default was 9 */
    cfg->agc_target_level_dbfs   = 3;   /* target -3 dBFS peak output */

    ESP_LOGI(TAG, "AFE linear gain=%.1f, software pre-gain=%.2f, AGC=%s (comp=%ddB target=-%ddBFS)",
             (double)cfg->afe_linear_gain, (double)s_sw_gain,
             cfg->agc_init ? "on" : "off",
             cfg->agc_compression_gain_db, cfg->agc_target_level_dbfs);

    /* Diagnostic: dump AFE config to verify WakeNet is configured */
    ESP_LOGI(TAG, "AFE config: wakenet_init=%d, wakenet_model='%s', "
             "vad_init=%d, se_init=%d, ns_init=%d, agc_init=%d, "
             "afe_type=%d, afe_mode=%d",
             cfg->wakenet_init,
             cfg->wakenet_model_name ? cfg->wakenet_model_name : "(null)",
             cfg->vad_init, cfg->se_init, cfg->ns_init, cfg->agc_init,
             cfg->afe_type, cfg->afe_mode);
    afe_config_print(cfg);

    s_afe_iface = esp_afe_handle_from_config(cfg);
    if (!s_afe_iface) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }

    s_afe_data = s_afe_iface->create_from_config(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    /* Apply WakeNet detection threshold from NVS (or default).
     * Range 0.0 – 0.9999.  Lower = more sensitive.
     * Index 1 = first wake word (API is 1-based: 1=wakenet1, 2=wakenet2). */
    int thr_ret = s_afe_iface->set_wakenet_threshold(s_afe_data, 1, s_wn_threshold);
    ESP_LOGI(TAG, "set_wakenet_threshold(1, %.3f) returned %d",
             (double)s_wn_threshold, thr_ret);
    ESP_LOGI(TAG, "WakeNet threshold set to %.3f", (double)s_wn_threshold);

    s_feed_chunk = s_afe_iface->get_feed_chunksize(s_afe_data);
    ESP_LOGI(TAG, "ESP-SR AFE init OK (feed_chunk=%d samples)", s_feed_chunk);
    return ESP_OK;
}

/* SRAM stack sizes — MUST be in internal SRAM (not PSRAM) because ESP-SR
 * reads model data from flash, and PSRAM stacks trigger the SPI flash
 * cache safety assertion.  Feed needs 8KB (DSP processing inside afe->feed).
 * Detect needs 8KB: calls audio_ring_open_wav (LittleFS), audio_ring_patch_wav_sizes,
 * audio_ring_commit, ws_server_broadcast_monitor (semaphore+socket) — 4KB overflows. */
#define WW_FEED_STACK    8192
#define WW_DETECT_STACK  8192

static StaticTask_t  s_feed_tcb;
static StaticTask_t  s_detect_tcb;
static StackType_t  *s_feed_stack  = NULL;
static StackType_t  *s_detect_stack = NULL;

esp_err_t wake_word_start(void)
{
    if (!s_afe_data) {
        esp_err_t ret = wake_word_init();
        if (ret != ESP_OK) return ret;
    }

    /* Allocate task stacks in SRAM (MALLOC_CAP_INTERNAL) — critical for flash access */
    if (!s_feed_stack) {
        s_feed_stack = heap_caps_malloc(WW_FEED_STACK, MALLOC_CAP_INTERNAL);
    }
    if (!s_detect_stack) {
        s_detect_stack = heap_caps_malloc(WW_DETECT_STACK, MALLOC_CAP_INTERNAL);
    }
    if (!s_feed_stack || !s_detect_stack) {
        ESP_LOGE(TAG, "Failed to alloc SRAM stacks for wake word tasks "
                 "(need %d+%d bytes, free SRAM=%"PRIu32")",
                 WW_FEED_STACK, WW_DETECT_STACK,
                 (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    s_feed_task_handle = xTaskCreateStaticPinnedToCore(
        feed_task, "ww_feed",
        WW_FEED_STACK, NULL, 6,
        s_feed_stack, &s_feed_tcb, 0);

    s_detect_task_handle = xTaskCreateStaticPinnedToCore(
        detect_task, "ww_detect",
        WW_DETECT_STACK, NULL, 5,
        s_detect_stack, &s_detect_tcb, 0);

    if (!s_feed_task_handle || !s_detect_task_handle) {
        ESP_LOGE(TAG, "Failed to create wake word tasks");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word tasks started ('Hi ESP' ready) — SRAM stacks: feed=%d detect=%d",
             WW_FEED_STACK, WW_DETECT_STACK);
    return ESP_OK;
}

void wake_word_suspend(void)
{
    /* Cooperative stop: set flag and wait for both tasks to park themselves.
     * This avoids the vTaskSuspend deadlock (task holds I2S DMA semaphore). */
    s_suspended = true;
    /* Wait up to 500ms for the feed task to finish its current I2S read
     * and park in the yield loop.  Each iteration is ~32ms (one feed chunk). */
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_LOGI(TAG, "Wake word suspended");
}

void wake_word_resume(void)
{
    s_suspended = false;
    ESP_LOGI(TAG, "Wake word resumed");
}

bool wake_word_is_running(void)
{
    return s_feed_task_handle != NULL;
}

void wake_word_set_gain(float gain)
{
    if (gain < 0.1f) gain = 0.1f;
    if (gain > 50.0f) gain = 50.0f;
    s_sw_gain = gain;
    ww_nvs_save_float(WW_NVS_KEY_GAIN, gain);
    ESP_LOGI(TAG, "Software pre-gain set to %.2f (saved to NVS)", (double)gain);
}

float wake_word_get_gain(void)
{
    return s_sw_gain;
}

void wake_word_set_threshold(float threshold)
{
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 0.9999f) threshold = 0.9999f;
    s_wn_threshold = threshold;
    ww_nvs_save_float(WW_NVS_KEY_THRESH, threshold);
    /* Apply immediately to the running AFE if available.
     * Index 1 = first wake word (API is 1-based). */
    if (s_afe_iface && s_afe_data) {
        s_afe_iface->set_wakenet_threshold(s_afe_data, 1, threshold);
        ESP_LOGI(TAG, "WakeNet threshold set to %.3f (live + NVS)", (double)threshold);
    } else {
        ESP_LOGI(TAG, "WakeNet threshold set to %.3f (saved to NVS, apply on next init)",
                 (double)threshold);
    }
}

float wake_word_get_threshold(void)
{
    return s_wn_threshold;
}

void wake_word_test_start(int seconds)
{
    /* Reset test counters */
    s_total_feeds   = 0;
    s_total_fetches = 0;
    s_total_speech  = 0;
    s_total_wakeups = 0;
    s_test_peak_pos = 0;
    s_test_peak_neg = 0;
    s_last_volume_db = -99.0f;
    s_test_seconds = (seconds > 0) ? seconds : 15;
    ESP_LOGI(TAG, "Test mode ON for %d seconds — say 'Hi ESP'", s_test_seconds);
}

void wake_word_test_stop(void)
{
    s_test_seconds = 0;
}

void wake_word_test_snapshot(wake_word_test_result_t *out)
{
    if (!out) return;
    out->feeds      = s_total_feeds;
    out->fetches    = s_total_fetches;
    out->speech     = s_total_speech;
    out->wakeups    = s_total_wakeups;
    out->volume_db  = s_last_volume_db;
    out->peak_pos   = s_test_peak_pos;
    out->peak_neg   = s_test_peak_neg;
    out->gain       = s_sw_gain;
    out->threshold  = s_wn_threshold;
}

/* ── Stub implementations when esp-sr is not available ────────── */

#else  /* !__has_include("esp_afe_sr_iface.h") */

esp_err_t wake_word_init(void)
{
    ESP_LOGW(TAG, "esp-sr not available — wake word disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wake_word_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void wake_word_suspend(void) {}
void wake_word_resume(void) {}
bool wake_word_is_running(void) { return false; }
void wake_word_set_gain(float gain) { (void)gain; }
float wake_word_get_gain(void) { return 1.0f; }
void wake_word_set_threshold(float threshold) { (void)threshold; }
float wake_word_get_threshold(void) { return 0.5f; }
void wake_word_test_start(int seconds) { (void)seconds; }
void wake_word_test_stop(void) {}
void wake_word_test_snapshot(wake_word_test_result_t *out) { if (out) memset(out, 0, sizeof(*out)); }

#endif /* __has_include("esp_afe_sr_iface.h") */
