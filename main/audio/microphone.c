#include "microphone.h"
#include "audio/i2s_audio.h"
#include "audio/audio_pipeline.h"
#include "led/led_indicator.h"
#include "langoustine_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "microphone";

/* chat_id used for all local PTT recordings */
#define PTT_CHAT_ID "ptt"

/* Maximum recording duration — auto-commits after this many ms */
#define PTT_MAX_DURATION_MS  8000

static TaskHandle_t s_ptt_task = NULL;

/* ── PTT task ────────────────────────────────────────────────── */

static void ptt_task(void *arg)
{
    ESP_LOGI(TAG, "PTT task started on Core %d (GPIO %d)", xPortGetCoreID(), LANG_PTT_GPIO);

    bool            was_pressed   = false;
    TickType_t      press_start   = 0;
    uint8_t         chunk[LANG_MIC_READ_CHUNK_BYTES];

    while (1) {
        bool pressed = (gpio_get_level(LANG_PTT_GPIO) == 0);

        if (pressed && !was_pressed) {
            /* ── Button just pressed ── */
            ESP_LOGI(TAG, "PTT pressed — opening WAV ring");
            esp_err_t ret = audio_ring_open_wav(PTT_CHAT_ID,
                                                LANG_MIC_SAMPLE_RATE, 1,
                                                LANG_MIC_BITS);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Cannot open audio ring: %s — ignoring press",
                         esp_err_to_name(ret));
                /* Wait for button release before retrying */
                while (gpio_get_level(LANG_PTT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                continue;
            }
            led_indicator_set(LED_LISTENING);
            press_start = xTaskGetTickCount();
            was_pressed = true;

        } else if (!pressed && was_pressed) {
            /* ── Button just released ── */
            ESP_LOGI(TAG, "PTT released — committing recording");
            audio_ring_patch_wav_sizes();
            audio_ring_commit(PTT_CHAT_ID);
            led_indicator_set(LED_THINKING);
            was_pressed = false;

        } else if (pressed && was_pressed) {
            /* ── Button held — read audio chunk ── */
            TickType_t elapsed = xTaskGetTickCount() - press_start;
            if (elapsed > pdMS_TO_TICKS(PTT_MAX_DURATION_MS)) {
                ESP_LOGW(TAG, "PTT max duration (%d s) — auto-committing",
                         PTT_MAX_DURATION_MS / 1000);
                audio_ring_patch_wav_sizes();
                audio_ring_commit(PTT_CHAT_ID);
                led_indicator_set(LED_THINKING);
                was_pressed = false;
                /* Wait for button release before allowing next recording */
                while (gpio_get_level(LANG_PTT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                continue;
            }

            size_t got = 0;
            esp_err_t ret = i2s_audio_read(chunk, sizeof(chunk), &got, 100);
            if (ret == ESP_OK && got > 0) {
                ret = audio_ring_append(chunk, got);
                if (ret == ESP_ERR_NO_MEM) {
                    /* Ring overflow — recording was dropped by audio_pipeline */
                    ESP_LOGW(TAG, "Ring overflow — recording dropped");
                    led_indicator_set(LED_ERROR);
                    was_pressed = false;
                    while (gpio_get_level(LANG_PTT_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
            }
            /* No delay when reading audio — loop immediately for continuous capture */
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));  /* 20 ms poll interval when idle */
    }
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t microphone_init(void)
{
    /* Configure GPIO 0 (BOOT button) as input with internal pull-up */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LANG_PTT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PTT GPIO %d configured (active low, pull-up)", LANG_PTT_GPIO);
    return ESP_OK;
}

esp_err_t microphone_start(void)
{
    if (s_ptt_task) {
        ESP_LOGW(TAG, "PTT task already running");
        return ESP_OK;
    }
    BaseType_t ret = xTaskCreatePinnedToCore(
        ptt_task, "ptt_task",
        LANG_MIC_STACK_SIZE, NULL,
        LANG_MIC_TASK_PRIO,
        &s_ptt_task,
        0);  /* Core 0 — keeps Core 1 free for agent */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PTT task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PTT task started");
    return ESP_OK;
}
