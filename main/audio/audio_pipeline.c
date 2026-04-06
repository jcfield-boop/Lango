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
#include "esp_heap_caps.h"

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
    char      channel[16]; /* message bus channel for STT result */
    SemaphoreHandle_t lock;  /* SRAM mutex */
} audio_ring_t;

static audio_ring_t s_ring = {0};
static TaskHandle_t s_stt_task_handle = NULL;

#include "audio/opus_encode.h"

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
        char   channel[16];
        strncpy(chat_id, s_ring.chat_id, sizeof(chat_id) - 1);
        strncpy(mime, s_ring.mime, sizeof(mime) - 1);
        strncpy(channel, s_ring.channel, sizeof(channel) - 1);
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

        /* For local mic audio (WAV), compress to Opus/OGG before STT upload.
         * This reduces upload size by ~10-15x (e.g. 160KB WAV → 12KB OGG).
         * Browser audio is already WebM/Opus — skip encoding for that path. */
        uint8_t *stt_buf = s_ring.buf;
        size_t   stt_len = audio_len;
        char     stt_mime[64];
        strncpy(stt_mime, mime, sizeof(stt_mime) - 1);
        stt_mime[sizeof(stt_mime) - 1] = '\0';
        uint8_t *opus_buf = NULL;

        if (strcmp(mime, "audio/wav") == 0 && audio_len > 44
            && stt_get_local_url()[0] == '\0') {
            /* WAV → Opus/OGG compression for cloud STT only (bandwidth matters).
             * Skip Opus encoding when local STT (mlx-audio) is configured:
             *   - LAN upload cost is negligible; raw WAV avoids the extra PSRAM
             *     allocation and CPU time in the STT task.
             *   - Opus encoder state is >4KB → allocated in PSRAM; the STT task
             *     stack is also PSRAM (24KB > SPIRAM_MALLOC_ALWAYSINTERNAL=4096).
             *     Two concurrent PSRAM-backed allocations + TLS stack pressure
             *     can cause panics on wake-word audio paths. */
            const int16_t *pcm = (const int16_t *)(s_ring.buf + 44);
            size_t pcm_bytes = audio_len - 44;
            size_t opus_size = 0;

            esp_err_t enc_ret = opus_encode_pcm_to_ogg(pcm, pcm_bytes,
                                                        LANG_MIC_SAMPLE_RATE,
                                                        &opus_buf, &opus_size);
            if (enc_ret == ESP_OK && opus_buf && opus_size > 0) {
                stt_buf = opus_buf;
                stt_len = opus_size;
                strncpy(stt_mime, "audio/ogg", sizeof(stt_mime) - 1);
                ESP_LOGI(TAG, "Opus encoding: %u → %u bytes (%.0f%% reduction)",
                         (unsigned)audio_len, (unsigned)opus_size,
                         (double)(100.0f - (float)opus_size / audio_len * 100.0f));
            } else {
                ESP_LOGW(TAG, "Opus encode failed (%s), sending raw WAV",
                         esp_err_to_name(enc_ret));
            }
        } else if (strcmp(mime, "audio/wav") == 0 && stt_get_local_url()[0] != '\0') {
            ESP_LOGI(TAG, "Local STT configured — skipping Opus encoding, sending raw WAV (%u bytes)",
                     (unsigned)audio_len);
        }

        stt_result_t result = {0};
        esp_err_t ret = stt_transcribe(stt_buf, stt_len, stt_mime, &result);

        /* Free Opus buffer if we allocated one */
        if (opus_buf) free(opus_buf);

        if (ret == ESP_OK && result.text[0] != '\0') {
            ESP_LOGI(TAG, "STT result: %s", result.text);
            ws_server_broadcast_monitor("stt_result", result.text);

            /* Push transcribed text to message bus using originating channel */
            lang_msg_t msg = {0};
            strncpy(msg.channel, channel[0] ? channel : LANG_CHAN_WEBSOCKET,
                    sizeof(msg.channel) - 1);
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

    /* STT task pinned to Core 1.  Stack in PSRAM — safe with XIP enabled.
     * Frees 24KB SRAM for WiFi/DNS/HTTP allocations. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        audio_stt_task, "stt_task",
        24 * 1024, NULL,  /* 24KB: needs >18KB (opus+http) */
        7,   /* STT_PRIO: higher than agent (6) to prevent starvation */
        &s_stt_task_handle,
        LANG_STT_CORE,
        MALLOC_CAP_SPIRAM);

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

esp_err_t audio_ring_open(const char *chat_id, const char *mime, const char *channel)
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
    strncpy(s_ring.channel, channel ? channel : LANG_CHAN_WEBSOCKET, sizeof(s_ring.channel) - 1);

    xSemaphoreGive(s_ring.lock);
    ESP_LOGI(TAG, "Audio ring opened: chat_id=%s mime=%s channel=%s",
             s_ring.chat_id, s_ring.mime, s_ring.channel);
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
    s_ring.committed = false;   /* allow future recordings */
    xSemaphoreGive(s_ring.lock);
    return ESP_OK;
}

esp_err_t audio_ring_open_wav(const char *chat_id, uint32_t sample_rate,
                               uint16_t channels, uint16_t bits,
                               const char *channel)
{
    /* Open the ring, then write a 44-byte WAV header with placeholder sizes */
    esp_err_t ret = audio_ring_open(chat_id, "audio/wav", channel);
    if (ret != ESP_OK) return ret;

    /* Standard 44-byte PCM WAV header; data chunk size is 0 (patched at commit) */
    uint32_t byte_rate  = sample_rate * channels * (bits / 8);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));

    uint8_t hdr[44] = {0};
    memcpy(hdr +  0, "RIFF", 4);
    /* RIFF chunk size placeholder — patched later */
    memcpy(hdr +  8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_sz  = 16;
    uint16_t pcm_fmt = 1;
    memcpy(hdr + 16, &fmt_sz,       4);
    memcpy(hdr + 20, &pcm_fmt,      2);
    memcpy(hdr + 22, &channels,     2);
    memcpy(hdr + 24, &sample_rate,  4);
    memcpy(hdr + 28, &byte_rate,    4);
    memcpy(hdr + 32, &block_align,  2);
    memcpy(hdr + 34, &bits,         2);
    memcpy(hdr + 36, "data", 4);
    /* data chunk size placeholder — patched later */

    return audio_ring_append(hdr, sizeof(hdr));
}

esp_err_t audio_ring_patch_wav_sizes(void)
{
    if (!s_ring.buf) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);

    if (!s_ring.active || s_ring.write_pos < 44) {
        xSemaphoreGive(s_ring.lock);
        ESP_LOGW(TAG, "audio_ring_patch_wav_sizes: ring not active or too small");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t data_bytes = (uint32_t)(s_ring.write_pos - 44);
    uint32_t riff_bytes = (uint32_t)(s_ring.write_pos - 8);

    memcpy(s_ring.buf +  4, &riff_bytes, 4);   /* RIFF chunk size */
    memcpy(s_ring.buf + 40, &data_bytes, 4);   /* data chunk size */

    xSemaphoreGive(s_ring.lock);
    ESP_LOGI(TAG, "WAV sizes patched: riff=%u data=%u",
             (unsigned)riff_bytes, (unsigned)data_bytes);
    return ESP_OK;
}
