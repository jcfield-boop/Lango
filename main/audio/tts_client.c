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
#include "freertos/task.h"

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

/* Local TTS (mlx-audio / Piper / etc.) — plain HTTP, no TLS */
#define TTS_LOCAL_URL_MAX    192
#define TTS_LOCAL_MODEL_MAX  128
#define TTS_LOCAL_VOICE_MAX   64
static char s_local_url[TTS_LOCAL_URL_MAX]     = {0};
static char s_local_model[TTS_LOCAL_MODEL_MAX] = "mlx-community/Kokoro-82M-bf16";
static char s_local_voice[TTS_LOCAL_VOICE_MAX] = "af_heart";
static bool s_local_offline = false;
static int64_t s_local_fail_us = 0;
#define LOCAL_BACKOFF_US   (60 * 1000000LL)  /* skip local for 60s after failure */
#define TTS_WU_TIMEOUT_MS  90000             /* Kokoro cold-load can take up to ~60s */

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

/* ── Boot warmup: pre-loads Kokoro model before first real TTS request ── */

static void tts_warmup_task(void *arg)
{
    /* Stagger after STT warmup (8s) and before LLM warmup (25s) */
    vTaskDelay(pdMS_TO_TICKS(18000));

    if (!s_local_url[0]) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TTS warmup: pre-loading Kokoro at %s", s_local_url);
    ws_server_broadcast_monitor_verbose("tts", "Pre-loading local Kokoro model...");

    char url[TTS_LOCAL_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/v1/audio/speech", s_local_url);

    /* One-word request — triggers model load without generating much audio */
    cJSON *req = cJSON_CreateObject();
    if (!req) { vTaskDelete(NULL); return; }
    cJSON_AddStringToObject(req, "model",           s_local_model[0] ? s_local_model : "mlx-community/Kokoro-82M-bf16");
    cJSON_AddStringToObject(req, "input",           "hi");
    cJSON_AddStringToObject(req, "voice",           s_local_voice[0] ? s_local_voice : "af_heart");
    cJSON_AddStringToObject(req, "response_format", "wav");

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) { vTaskDelete(NULL); return; }

    /* bin_buf_t with no pre-allocation — bin_append will ps_malloc on first chunk */
    bin_buf_t bb = {0};

    int64_t t0 = esp_timer_get_time();

    esp_http_client_config_t cfg = {
        .url            = url,
        .event_handler  = http_event_cb,
        .user_data      = &bb,
        .timeout_ms     = TTS_WU_TIMEOUT_MS,
        .buffer_size    = 8192,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);
    if (bb.data) free(bb.data);  /* ps_malloc'd PSRAM — standard free() works */

    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    if (err == ESP_OK && status == 200) {
        s_local_offline = false;
        ESP_LOGI(TAG, "TTS warmup done in %lldms — Kokoro ready", elapsed_ms);
        ws_server_broadcast_monitor("tts", "Local TTS ready (warmed up)");
    } else {
        ESP_LOGW(TAG, "TTS warmup failed after %lldms (err=%s, HTTP %d) — will retry on first use",
                 elapsed_ms, esp_err_to_name(err), status);
    }

    vTaskDelete(NULL);
}

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

    /* Pre-load Kokoro at boot (18s delay) to avoid 60s cold-start on first request.
     * Task checks s_local_url after delay — set by services_config_load() at boot. */
    xTaskCreate(tts_warmup_task, "tts_warmup", 6 * 1024, NULL, 2, NULL);

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

/* ── Local TTS (plain HTTP, no TLS) ────────────────────────────── */

static bool local_tts_available(void)
{
    if (!s_local_url[0]) return false;
    if (s_local_offline) {
        if ((esp_timer_get_time() - s_local_fail_us) < LOCAL_BACKOFF_US) return false;
        s_local_offline = false;  /* retry after backoff */
    }
    return true;
}

/**
 * Try generating TTS via local server (mlx-audio compatible).
 * Returns ESP_OK + fills bb on success, ESP_FAIL on any error.
 */
static esp_err_t tts_generate_local(const char *text, bin_buf_t *bb)
{
    char url[TTS_LOCAL_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/v1/audio/speech", s_local_url);

    cJSON *req = cJSON_CreateObject();
    if (!req) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(req, "model", s_local_model);
    cJSON_AddStringToObject(req, "input", text);
    cJSON_AddStringToObject(req, "voice", s_local_voice);
    cJSON_AddStringToObject(req, "response_format", "wav");

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url            = url,
        .event_handler  = http_event_cb,
        .user_data      = bb,
        .timeout_ms     = 60000,  /* local LAN — 60s for long text (Kokoro ~2-3s/sentence warm) */
        .buffer_size    = 8192,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    ESP_LOGI(TAG, "TTS local: POST %s (%u chars)", url, (unsigned)strlen(text));
    ws_server_broadcast_monitor_verbose("tts", "Trying local TTS (mlx-audio)...");

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (ret != ESP_OK || status != 200 || bb->len == 0) {
        ESP_LOGW(TAG, "Local TTS failed: %s (HTTP %d, %u bytes)",
                 esp_err_to_name(ret), status, (unsigned)bb->len);
        s_local_offline = true;
        s_local_fail_us = esp_timer_get_time();
        ws_server_broadcast_monitor_verbose("tts", "Local TTS offline, falling back to cloud");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Local TTS OK: %u bytes", (unsigned)bb->len);
    ws_server_broadcast_monitor_verbose("tts", "Local TTS success");
    return ESP_OK;
}

esp_err_t tts_generate(const char *text, char *id_out)
{
    if (!text || !text[0] || !id_out) return ESP_ERR_INVALID_ARG;

    /* Allow TTS if we have either a cloud API key or a local URL */
    if (!s_api_key[0] && !s_local_url[0]) {
        ESP_LOGW(TAG, "No TTS API key and no local URL — skipping TTS");
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
    {
        char tts_info[120];
        snprintf(tts_info, sizeof(tts_info), "Speaking: \"%.100s%s\"",
                 text, strlen(text) > 100 ? "..." : "");
        ws_server_broadcast_monitor_verbose("tts", tts_info);
    }

    /* Binary response buffer, starting at 64KB in PSRAM */
    bin_buf_t bb = {0};
    bb.data = ps_malloc(TTS_GROW_STEP);
    if (!bb.data) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    bb.cap = TTS_GROW_STEP;

    bool got_audio = false;
    esp_err_t ret;

    /* ── Try local TTS first (plain HTTP, no TLS) ─────────────── */
    if (local_tts_available()) {
        ret = tts_generate_local(text, &bb);
        if (ret == ESP_OK && bb.len > 0 && !bb.oom) {
            got_audio = true;
        } else {
            /* Reset buffer for cloud fallback */
            bb.len = 0;
            bb.oom = false;
        }
    }

    /* ── Cloud TTS (Groq) ──────────────────────────────────────── */
    if (!got_audio) {
        if (!s_api_key[0]) {
            ESP_LOGW(TAG, "No TTS API key — skipping cloud TTS");
            free(body);
            free(bb.data);
            return ESP_ERR_INVALID_STATE;
        }

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

        http_session_set_ctx(&s_session, &bb);
        esp_http_client_set_method(s_session.handle, HTTP_METHOD_POST);
        esp_http_client_set_header(s_session.handle, "Authorization", auth);
        esp_http_client_set_header(s_session.handle, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_session.handle, body, (int)strlen(body));

        ret = esp_http_client_perform(s_session.handle);

        if (ret == ESP_ERR_HTTP_CONNECT || ret == ESP_ERR_HTTP_CONNECTION_CLOSED) {
            ESP_LOGW(TAG, "TTS connection lost (%s), resetting and retrying", esp_err_to_name(ret));
            ws_server_broadcast_monitor_verbose("tts", "Connection lost, retrying...");
            http_session_reset(&s_session);
            if (s_session.valid) {
                http_session_set_ctx(&s_session, &bb);
                esp_http_client_set_method(s_session.handle, HTTP_METHOD_POST);
                esp_http_client_set_header(s_session.handle, "Authorization", auth);
                esp_http_client_set_header(s_session.handle, "Content-Type", "application/json");
                esp_http_client_set_post_field(s_session.handle, body, (int)strlen(body));
                bb.len = 0;
                ret = esp_http_client_perform(s_session.handle);
            }
        }

        int status = esp_http_client_get_status_code(s_session.handle);
        free(body);
        body = NULL;

        if (ret != ESP_OK || status != 200) {
            char tts_err[80];
            if (bb.data && bb.len > 0) {
                size_t print_len = bb.len < 255 ? bb.len : 255;
                bb.data[print_len] = '\0';
                ESP_LOGE(TAG, "TTS HTTP %d body: %s", status, (char *)bb.data);
                snprintf(tts_err, sizeof(tts_err), "TTS failed: HTTP %d", status);
            } else {
                ESP_LOGE(TAG, "TTS HTTP %d (no body): %s", status, esp_err_to_name(ret));
                snprintf(tts_err, sizeof(tts_err), "TTS failed: %s", esp_err_to_name(ret));
            }
            ws_server_broadcast_monitor_verbose("tts", tts_err);
            free(bb.data);
            return (ret != ESP_OK) ? ret : ESP_FAIL;
        }

        got_audio = true;
    }
    free(body);  /* safe if already NULL */

    if (bb.oom || bb.len == 0) {
        ESP_LOGE(TAG, "TTS response buffer error (len=%u, oom=%d)", (unsigned)bb.len, bb.oom);
        free(bb.data);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TTS received: %u bytes audio", (unsigned)bb.len);
    {
        char tts_ok[48];
        snprintf(tts_ok, sizeof(tts_ok), "Received %uKB audio", (unsigned)(bb.len / 1024));
        ws_server_broadcast_monitor_verbose("tts", tts_ok);
    }

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

void tts_set_local_url(const char *url)
{
    if (!url) { s_local_url[0] = '\0'; return; }
    strncpy(s_local_url, url, sizeof(s_local_url) - 1);
    s_local_url[sizeof(s_local_url) - 1] = '\0';
    /* Strip trailing slash */
    size_t len = strlen(s_local_url);
    if (len > 0 && s_local_url[len - 1] == '/') s_local_url[len - 1] = '\0';
    s_local_offline = false;
    ESP_LOGI(TAG, "Local TTS URL: %s", s_local_url);
}

const char *tts_get_local_url(void)
{
    return s_local_url;
}

void tts_set_local_model(const char *model)
{
    if (!model || model[0] == '\0') return;
    strncpy(s_local_model, model, sizeof(s_local_model) - 1);
    s_local_model[sizeof(s_local_model) - 1] = '\0';
    ESP_LOGI(TAG, "Local TTS model: %s", s_local_model);
}

void tts_set_local_voice(const char *voice)
{
    if (!voice || voice[0] == '\0') return;
    strncpy(s_local_voice, voice, sizeof(s_local_voice) - 1);
    s_local_voice[sizeof(s_local_voice) - 1] = '\0';
    ESP_LOGI(TAG, "Local TTS voice: %s", s_local_voice);
}
