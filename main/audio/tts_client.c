#include "tts_client.h"
#include "langoustine_config.h"
#include "memory/psram_alloc.h"
#include "llm/http_session.h"
#include "gateway/ws_server.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tts";

#define TTS_API_KEY_MAX   320
#define TTS_ENDPOINT_MAX  256
#define TTS_MODEL_MAX     64
#define TTS_VOICE_MAX     64
#define TTS_TTL_S         900   /* 15-minute cache TTL */
#define TTS_GROW_STEP     (64 * 1024)

/* ── In-memory PSRAM cache ─────────────────────────────────────── */

typedef struct {
    char     id[9];       /* 8 hex chars + '\0' */
    uint8_t *buf;         /* ps_malloc'd PSRAM buffer */
    size_t   len;
    int64_t  created_us;  /* esp_timer_get_time() at generation */
    bool     active;
} tts_cache_entry_t;

static tts_cache_entry_t s_cache[LANG_TTS_CACHE_MAX];
static SemaphoreHandle_t s_cache_lock = NULL;

static char s_api_key[TTS_API_KEY_MAX]   = {0};
static char s_endpoint[TTS_ENDPOINT_MAX] = LANG_DEFAULT_TTS_ENDPOINT;
static char s_model[TTS_MODEL_MAX]       = LANG_DEFAULT_TTS_MODEL;
static char s_voice[TTS_VOICE_MAX]       = LANG_DEFAULT_TTS_VOICE;

/* Persistent HTTP session — created once in tts_client_init() */
static http_session_t s_session = {0};

/* ── Response accumulator (binary, PSRAM) ──────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     oom;
} bin_buf_t;

static esp_err_t bin_append(bin_buf_t *bb, const uint8_t *src, size_t n)
{
    if (bb->oom) return ESP_ERR_NO_MEM;
    if (bb->len + n > bb->cap) {
        size_t new_cap = bb->cap + n + TTS_GROW_STEP;
        uint8_t *tmp = ps_realloc(bb->data, new_cap);
        if (!tmp) {
            bb->oom = true;
            return ESP_ERR_NO_MEM;
        }
        bb->data = tmp;
        bb->cap  = new_cap;
    }
    memcpy(bb->data + bb->len, src, n);
    bb->len += n;
    return ESP_OK;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    bin_buf_t *bb = (bin_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && bb) {
        bin_append(bb, (const uint8_t *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* ── Cache helpers ─────────────────────────────────────────────── */

static void cache_evict_oldest(void)
{
    /* Find the oldest active entry and free it */
    int oldest_idx = -1;
    int64_t oldest_time = INT64_MAX;

    for (int i = 0; i < LANG_TTS_CACHE_MAX; i++) {
        if (s_cache[i].active && s_cache[i].created_us < oldest_time) {
            oldest_time = s_cache[i].created_us;
            oldest_idx  = i;
        }
    }

    if (oldest_idx >= 0) {
        ESP_LOGI(TAG, "TTS cache evict: %s", s_cache[oldest_idx].id);
        free(s_cache[oldest_idx].buf);
        s_cache[oldest_idx].buf    = NULL;
        s_cache[oldest_idx].len    = 0;
        s_cache[oldest_idx].active = false;
    }
}

static void cache_expire_old(void)
{
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < LANG_TTS_CACHE_MAX; i++) {
        if (s_cache[i].active) {
            int64_t age_s = (now_us - s_cache[i].created_us) / 1000000LL;
            if (age_s > TTS_TTL_S) {
                ESP_LOGI(TAG, "TTS cache expire: %s (age %llds)", s_cache[i].id, (long long)age_s);
                free(s_cache[i].buf);
                s_cache[i].buf    = NULL;
                s_cache[i].len    = 0;
                s_cache[i].active = false;
            }
        }
    }
}

static int cache_find_slot(void)
{
    for (int i = 0; i < LANG_TTS_CACHE_MAX; i++) {
        if (!s_cache[i].active) return i;
    }
    return -1;
}

static void gen_id(char *id_out)
{
    uint32_t r = esp_random();
    snprintf(id_out, 9, "%08lx", (unsigned long)r);
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t tts_client_init(void)
{
    s_cache_lock = xSemaphoreCreateMutex();
    if (!s_cache_lock) {
        ESP_LOGE(TAG, "Failed to create TTS cache mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_TTS, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[TTS_API_KEY_MAX];
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, LANG_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        }
        char etmp[TTS_ENDPOINT_MAX];
        len = sizeof(etmp);
        if (nvs_get_str(nvs, "endpoint", etmp, &len) == ESP_OK && etmp[0]) {
            strncpy(s_endpoint, etmp, sizeof(s_endpoint) - 1);
        }
        char mtmp[TTS_MODEL_MAX];
        len = sizeof(mtmp);
        if (nvs_get_str(nvs, "model", mtmp, &len) == ESP_OK && mtmp[0]) {
            strncpy(s_model, mtmp, sizeof(s_model) - 1);
        }
        char vtmp[TTS_VOICE_MAX];
        len = sizeof(vtmp);
        if (nvs_get_str(nvs, LANG_NVS_KEY_VOICE, vtmp, &len) == ESP_OK && vtmp[0]) {
            strncpy(s_voice, vtmp, sizeof(s_voice) - 1);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "TTS client ready (model: %s, voice: %s)", s_model, s_voice);
    } else {
        ESP_LOGW(TAG, "No TTS API key. Use CLI: tts_key <KEY>");
    }

    /* Create persistent session for TLS reuse */
    if (!s_session.valid) {
        esp_err_t err = http_session_init(&s_session, s_endpoint,
                                          http_event_cb,
                                          60 * 1000, 8192, 4096);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TTS http_session_init failed: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t tts_generate(const char *text, char *id_out)
{
    if (!text || !text[0] || !id_out) return ESP_ERR_INVALID_ARG;

    if (!s_api_key[0]) {
        ESP_LOGW(TAG, "No TTS API key — skipping TTS");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build JSON request body */
    cJSON *req = cJSON_CreateObject();
    if (!req) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(req, "model",           s_model[0] ? s_model : LANG_DEFAULT_TTS_MODEL);
    cJSON_AddStringToObject(req, "input",           text);
    cJSON_AddStringToObject(req, "voice",           s_voice[0] ? s_voice : LANG_DEFAULT_TTS_VOICE);
    cJSON_AddStringToObject(req, "response_format", "wav");

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "TTS generate: %u text chars", (unsigned)strlen(text));
    ws_server_broadcast_monitor_verbose("tts", "Groq PlayAI");

    /* Binary response buffer, starting at 64KB in PSRAM */
    bin_buf_t bb = {0};
    bb.data = ps_malloc(TTS_GROW_STEP);
    if (!bb.data) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    bb.cap = TTS_GROW_STEP;

    char auth[TTS_API_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);

    /* Reuse persistent session (lazy init fallback if init was skipped) */
    if (!s_session.valid) {
        http_session_init(&s_session, s_endpoint, http_event_cb,
                          60 * 1000, 8192, 4096);
    }
    if (!s_session.valid) {
        free(body);
        free(bb.data);
        return ESP_FAIL;
    }

    /* Point session at this call's response buffer */
    http_session_set_ctx(&s_session, &bb);

    esp_http_client_set_method(s_session.handle, HTTP_METHOD_POST);
    esp_http_client_set_header(s_session.handle, "Authorization", auth);
    esp_http_client_set_header(s_session.handle, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_session.handle, body, (int)strlen(body));

    esp_err_t ret = http_session_perform(&s_session);  /* auto-retry on drop */
    int status    = esp_http_client_get_status_code(s_session.handle);
    /* Do NOT cleanup — keep handle alive for TLS session reuse */
    free(body);

    if (ret != ESP_OK || status != 200) {
        if (bb.data && bb.len > 0) {
            size_t print_len = bb.len < 255 ? bb.len : 255;
            bb.data[print_len] = '\0';
            ESP_LOGE(TAG, "TTS HTTP %d body: %s", status, (char *)bb.data);
        } else {
            ESP_LOGE(TAG, "TTS HTTP %d (no body): %s", status, esp_err_to_name(ret));
        }
        free(bb.data);
        return (ret != ESP_OK) ? ret : ESP_FAIL;
    }

    if (bb.oom || bb.len == 0) {
        ESP_LOGE(TAG, "TTS response buffer error (len=%u, oom=%d)", (unsigned)bb.len, bb.oom);
        free(bb.data);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TTS received: %u bytes WAV", (unsigned)bb.len);

    /* Store in cache */
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);

    cache_expire_old();

    int slot = cache_find_slot();
    if (slot < 0) {
        cache_evict_oldest();
        slot = cache_find_slot();
    }

    if (slot < 0) {
        /* Still no slot — this shouldn't happen, but be safe */
        xSemaphoreGive(s_cache_lock);
        free(bb.data);
        return ESP_ERR_NO_MEM;
    }

    gen_id(s_cache[slot].id);
    s_cache[slot].buf        = bb.data;
    s_cache[slot].len        = bb.len;
    s_cache[slot].created_us = esp_timer_get_time();
    s_cache[slot].active     = true;

    strncpy(id_out, s_cache[slot].id, 9);
    id_out[8] = '\0';

    xSemaphoreGive(s_cache_lock);

    ESP_LOGI(TAG, "TTS cached: id=%s, %u bytes", id_out, (unsigned)bb.len);

    return ESP_OK;
}

esp_err_t tts_cache_get(const char *id, const uint8_t **buf_out, size_t *len_out)
{
    if (!id || !buf_out || !len_out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);

    int64_t now_us = esp_timer_get_time();
    esp_err_t result = ESP_ERR_NOT_FOUND;

    for (int i = 0; i < LANG_TTS_CACHE_MAX; i++) {
        if (!s_cache[i].active) continue;
        if (strcmp(s_cache[i].id, id) != 0) continue;

        int64_t age_s = (now_us - s_cache[i].created_us) / 1000000LL;
        if (age_s > TTS_TTL_S) {
            /* Expired */
            free(s_cache[i].buf);
            s_cache[i].buf    = NULL;
            s_cache[i].len    = 0;
            s_cache[i].active = false;
            break;
        }

        *buf_out = s_cache[i].buf;
        *len_out = s_cache[i].len;
        result   = ESP_OK;
        break;
    }

    xSemaphoreGive(s_cache_lock);
    return result;
}

void tts_set_api_key(const char *key)
{
    if (!key) return;
    strncpy(s_api_key, key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_TTS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, LANG_NVS_KEY_API_KEY, key);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "TTS API key saved to NVS");
    }
}

void tts_set_voice(const char *voice)
{
    if (!voice) return;
    strncpy(s_voice, voice, sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_TTS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, LANG_NVS_KEY_VOICE, voice);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "TTS voice set to: %s", voice);
    }
}

void tts_set_model(const char *model)
{
    if (!model) return;
    strncpy(s_model, model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_TTS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "model", model);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "TTS model set to: %s", model);
    }
}

void tts_get_api_key_masked(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    size_t key_len = strlen(s_api_key);
    if (key_len == 0) {
        strncpy(buf, "(not set)", buf_len - 1);
    } else if (key_len <= 6) {
        strncpy(buf, "****", buf_len - 1);
    } else {
        snprintf(buf, buf_len, "%.4s****", s_api_key);
    }
}

void tts_get_voice(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    strncpy(buf, s_voice[0] ? s_voice : LANG_DEFAULT_TTS_VOICE, buf_len - 1);
    buf[buf_len - 1] = '\0';
}
