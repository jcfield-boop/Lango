#include "audio_pipeline.h"
#include "audio/stt_client.h"
#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "audio_pipeline";

/* ── Ring buffer (PSRAM) ─────────────────────────────────────── */

typedef struct {
    uint8_t  *buf;
    size_t    write_pos;
    size_t    capacity;
    bool      active;      /* open for writing */
    bool      committed;   /* handed to STT task, buffer must not be touched */
    char      chat_id[32];
    char      mime[64];
    SemaphoreHandle_t lock;  /* SRAM mutex */
} audio_ring_t;

static audio_ring_t s_ring = {0};
static TaskHandle_t s_stt_task_handle = NULL;

/* ── Rate limiting ───────────────────────────────────────────── */

static int64_t s_last_stt_req_us = 0;

static bool rate_limit_ok(void)
{
    int64_t now = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS * 1000LL);
    int64_t min_gap_us = (int64_t)(60000000LL / LANG_RATE_LIMIT_RPM);
    if (s_last_stt_req_us > 0 && (now - s_last_stt_req_us) < min_gap_us) {
        return false;
    }
    s_last_stt_req_us = now;
    return true;
}

/* ── STT task (Core 1) ───────────────────────────────────────── */

static void audio_stt_task(void *arg)
{
    ESP_LOGI(TAG, "STT task started on Core %d", xPortGetCoreID());

    while (1) {
        /* Wait for signal from audio_ring_commit() */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(s_ring.lock, portMAX_DELAY);
        size_t audio_len = s_ring.write_pos;
        char   chat_id[32];
        char   mime[64];
        strncpy(chat_id, s_ring.chat_id, sizeof(chat_id) - 1);
        strncpy(mime, s_ring.mime, sizeof(mime) - 1);
        xSemaphoreGive(s_ring.lock);

        if (audio_len == 0) {
            ESP_LOGW(TAG, "STT task woken with empty buffer");
            continue;
        }

        /* Rate limiting */
        if (!rate_limit_ok()) {
            ESP_LOGW(TAG, "STT rate limit hit for %s", chat_id);
            ws_server_send_error(chat_id, "rate_limited", "Too many requests");
            audio_ring_reset_for_client(chat_id);
            continue;
        }

        ws_server_send_status(chat_id, "stt_processing");

        stt_result_t result = {0};
        esp_err_t ret = stt_transcribe(s_ring.buf, audio_len, mime, &result);

        if (ret == ESP_OK && result.text[0] != '\0') {
            ESP_LOGI(TAG, "STT result: %s", result.text);

            /* Push transcribed text to message bus as inbound WS prompt */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, LANG_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(result.text);
            if (msg.content) {
                if (message_bus_push_inbound(&msg) != ESP_OK) {
                    ESP_LOGW(TAG, "Inbound bus full, drop STT result for %s", chat_id);
                    free(msg.content);
                } else {
                    ws_server_send_status(chat_id, "llm_thinking");
                }
            }
        } else {
            ESP_LOGE(TAG, "STT failed: %s", esp_err_to_name(ret));
            ws_server_send_error(chat_id, "stt_failed",
                                 result.error[0] ? result.error : "Transcription failed");
            ws_server_send_status(chat_id, "idle");
        }

        audio_ring_reset_for_client(chat_id);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t audio_pipeline_init(void)
{
    /* Allocate ring buffer in PSRAM */
    s_ring.buf = ps_malloc(LANG_AUDIO_RING_SIZE);
    if (!s_ring.buf) {
        ESP_LOGE(TAG, "Failed to allocate audio ring buffer (%u bytes)", (unsigned)LANG_AUDIO_RING_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_ring.capacity  = LANG_AUDIO_RING_SIZE;
    s_ring.write_pos = 0;
    s_ring.active    = false;

    /* Mutex in SRAM */
    s_ring.lock = xSemaphoreCreateMutex();
    if (!s_ring.lock) {
        ESP_LOGE(TAG, "Failed to create audio ring mutex");
        free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* STT task pinned to Core 1 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_stt_task, "stt_task",
        12 * 1024, NULL,
        7,   /* STT_PRIO: higher than agent (6) to prevent starvation */
        &s_stt_task_handle,
        LANG_STT_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create STT task");
        vSemaphoreDelete(s_ring.lock);
        free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized: %u KB ring buffer in PSRAM",
             (unsigned)(LANG_AUDIO_RING_SIZE / 1024));
    return ESP_OK;
}

esp_err_t audio_ring_open(const char *chat_id, const char *mime)
{
    if (!s_ring.buf) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);

    if (s_ring.committed) {
        /* STT task is actively reading the buffer — must not overwrite */
        xSemaphoreGive(s_ring.lock);
        ESP_LOGW(TAG, "audio_ring_open: STT in progress, rejecting new session");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ring.active) {
        ESP_LOGW(TAG, "audio_ring_open: discarding uncommitted session");
    }

    s_ring.write_pos = 0;
    s_ring.active    = true;
    s_ring.committed = false;
    strncpy(s_ring.chat_id, chat_id ? chat_id : "", sizeof(s_ring.chat_id) - 1);
    strncpy(s_ring.mime, mime ? mime : "audio/webm;codecs=opus", sizeof(s_ring.mime) - 1);

    xSemaphoreGive(s_ring.lock);
    ESP_LOGI(TAG, "Audio ring opened: chat_id=%s mime=%s", s_ring.chat_id, s_ring.mime);
    return ESP_OK;
}

esp_err_t audio_ring_append(const uint8_t *data, size_t len)
{
    if (!s_ring.buf || !data || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);

    if (!s_ring.active) {
        xSemaphoreGive(s_ring.lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Overflow guard */
    if (s_ring.write_pos + len > LANG_AUDIO_MAX_UPLOAD_BYTES) {
        ESP_LOGW(TAG, "Audio ring overflow (%u + %u > %u), resetting",
                 (unsigned)s_ring.write_pos, (unsigned)len,
                 (unsigned)LANG_AUDIO_MAX_UPLOAD_BYTES);
        s_ring.write_pos = 0;
        s_ring.active    = false;
        xSemaphoreGive(s_ring.lock);
        ws_server_send_error(s_ring.chat_id, "audio_overflow", "Audio too long");
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_ring.buf + s_ring.write_pos, data, len);
    s_ring.write_pos += len;

    xSemaphoreGive(s_ring.lock);
    return ESP_OK;
}

esp_err_t audio_ring_commit(const char *chat_id)
{
    if (!s_ring.buf) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);

    if (!s_ring.active || s_ring.write_pos == 0) {
        xSemaphoreGive(s_ring.lock);
        ESP_LOGW(TAG, "audio_ring_commit: nothing to commit");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Audio committed: %u bytes for %s", (unsigned)s_ring.write_pos,
             s_ring.chat_id);

    s_ring.active    = false;   /* no more appends allowed */
    s_ring.committed = true;    /* buffer is now owned by STT task */

    xSemaphoreGive(s_ring.lock);

    /* Signal the STT task on Core 1 */
    if (s_stt_task_handle) {
        xTaskNotifyGive(s_stt_task_handle);
    }

    return ESP_OK;
}

esp_err_t audio_ring_reset(void)
{
    if (!s_ring.buf) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    s_ring.write_pos = 0;
    s_ring.active    = false;
    s_ring.committed = false;
    xSemaphoreGive(s_ring.lock);
    return ESP_OK;
}

esp_err_t audio_ring_reset_for_client(const char *chat_id)
{
    if (!s_ring.buf) return ESP_ERR_INVALID_STATE;
    if (!chat_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    if (s_ring.chat_id[0] != '\0' && strcmp(s_ring.chat_id, chat_id) != 0) {
        xSemaphoreGive(s_ring.lock);
        ESP_LOGW(TAG, "audio_ring_reset_for_client: chat_id mismatch (owner=%s, caller=%s)",
                 s_ring.chat_id, chat_id);
        return ESP_ERR_NOT_FOUND;
    }
    s_ring.write_pos = 0;
    s_ring.active    = false;
    xSemaphoreGive(s_ring.lock);
    return ESP_OK;
}
