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
#include "langoustine_config.h"
#include "memory/psram_alloc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

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

/* ms of VAD silence after speech to trigger end-of-utterance */
#define WW_VAD_SILENCE_MS       1500

/* Max recording duration (safety limit) */
#define WW_MAX_RECORD_MS        8000

/* ── Module state ─────────────────────────────────────────────── */

static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static esp_afe_sr_data_t        *s_afe_data  = NULL;
static int                       s_feed_chunk = 0;   /* samples per feed() call */

/* Shared between feed_task and detect_task (detect_task reads PTT too) */
static TaskHandle_t s_feed_task_handle   = NULL;
static TaskHandle_t s_detect_task_handle = NULL;

/* ── Feed task (Core 0) ──────────────────────────────────────── */

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "AFE feed task started (chunk=%d samples)", s_feed_chunk);

    int16_t *buf = (int16_t *)ps_malloc(s_feed_chunk * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "feed buf alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t got = 0;
        esp_err_t ret = i2s_audio_read((uint8_t *)buf,
                                       s_feed_chunk * sizeof(int16_t),
                                       &got, 200);
        if (ret == ESP_OK && got == (size_t)(s_feed_chunk * sizeof(int16_t))) {
            s_afe_iface->feed(s_afe_data, buf);
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

    while (1) {
        /* ── Check PTT button ────────────────────────────────── */
        bool ptt = (gpio_get_level(LANG_PTT_GPIO) == 0);

        if (ptt && !ptt_active && !recording) {
            /* PTT just pressed — start recording immediately */
            if (audio_ring_open_wav(WW_CHAT_ID, LANG_MIC_SAMPLE_RATE, 1,
                                     LANG_MIC_BITS) == ESP_OK) {
                led_indicator_set(LED_LISTENING);
                recording      = true;
                ptt_active     = true;
                speech_started = true;  /* PTT → no wait-for-speech timeout */
                activate_tick  = xTaskGetTickCount();
                silence_start  = 0;
                ESP_LOGI(TAG, "PTT pressed — recording");
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

        /* ── Wake word detection ─────────────────────────────── */
        if (!recording && !ptt_active &&
            res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word 'Hi ESP' detected");
            if (audio_ring_open_wav(WW_CHAT_ID, LANG_MIC_SAMPLE_RATE, 1,
                                     LANG_MIC_BITS) == ESP_OK) {
                led_indicator_set(LED_LISTENING);
                recording      = true;
                speech_started = false;
                activate_tick  = xTaskGetTickCount();
                silence_start  = 0;
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
                            ESP_LOGI(TAG, "VAD silence — committing");
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

    s_feed_chunk = s_afe_iface->get_feed_chunksize(s_afe_data);
    ESP_LOGI(TAG, "ESP-SR AFE init OK (feed_chunk=%d samples)", s_feed_chunk);
    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (!s_afe_data) {
        esp_err_t ret = wake_word_init();
        if (ret != ESP_OK) return ret;
    }

    BaseType_t r1 = xTaskCreatePinnedToCore(
        feed_task, "ww_feed",
        4096, NULL, 6,       /* slightly higher than detect to ensure continuous feeding */
        &s_feed_task_handle, 0);

    BaseType_t r2 = xTaskCreatePinnedToCore(
        detect_task, "ww_detect",
        4096, NULL, 5,
        &s_detect_task_handle, 0);

    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake word tasks");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word tasks started ('Hi ESP' ready)");
    return ESP_OK;
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

#endif /* __has_include("esp_afe_sr_iface.h") */
