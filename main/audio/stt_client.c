#include "stt_client.h"
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
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "stt";

#define STT_API_KEY_MAX  320
#define STT_ENDPOINT_MAX 256
#define STT_MODEL_MAX    64
#define STT_BOUNDARY     "langoustine_stt_42"

static char s_api_key[STT_API_KEY_MAX]  = {0};
static char s_endpoint[STT_ENDPOINT_MAX] = LANG_DEFAULT_STT_ENDPOINT;
static char s_model[STT_MODEL_MAX]       = LANG_DEFAULT_STT_MODEL;

/* Local STT (mlx-audio / whisper.cpp / etc.) — plain HTTP, no TLS */
#define STT_LOCAL_URL_MAX  192
static char s_local_url[STT_LOCAL_URL_MAX] = {0};
static bool s_local_offline = false;
static int64_t s_local_fail_us = 0;
#define LOCAL_BACKOFF_US   (60 * 1000000LL)  /* skip local for 60s after failure */

/* Persistent HTTP session — created once in stt_client_init() */
static http_session_t s_session = {0};

/* ── Response accumulator ──────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} resp_buf_t;

static esp_err_t resp_append(resp_buf_t *rb, const char *src, size_t n)
{
    if (rb->oom) return ESP_ERR_NO_MEM;
    if (rb->len + n >= rb->cap) {
        size_t new_cap = rb->cap + n + 1024;
        char *tmp = ps_realloc(rb->data, new_cap);  /* P3-1: use ps_realloc */
        if (!tmp) {
            rb->oom = true;
            return ESP_ERR_NO_MEM;
        }
        rb->data = tmp;
        rb->cap  = new_cap;
    }
    memcpy(rb->data + rb->len, src, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        resp_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* ── Multipart body builder ────────────────────────────────────── */

/* Derive a safe filename extension from the MIME type.
 * "audio/webm;codecs=opus" → "webm", "audio/ogg" → "ogg", etc. */
static void mime_to_ext(const char *mime, char *ext, size_t ext_size)
{
    const char *slash = strchr(mime, '/');
    if (!slash) {
        strncpy(ext, "webm", ext_size - 1);
        return;
    }
    slash++;
    size_t i = 0;
    while (slash[i] && slash[i] != ';' && slash[i] != ' ' && i < ext_size - 1) {
        ext[i] = slash[i];
        i++;
    }
    ext[i] = '\0';
    if (ext[0] == '\0') strncpy(ext, "webm", ext_size - 1);
}

static uint8_t *build_multipart(const uint8_t *audio, size_t audio_len,
                                 const char *mime, const char *model,
                                 size_t *total_len)
{
    char ext[16] = {0};
    mime_to_ext(mime, ext, sizeof(ext));

    /* Calculate header sizes before allocating */
    char hdr_file[256];
    int hdr_file_len = snprintf(hdr_file, sizeof(hdr_file),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.%s\"\r\n"
        "Content-Type: %s\r\n"
        "\r\n",
        STT_BOUNDARY, ext, mime);

    char hdr_model[256];
    int hdr_model_len = snprintf(hdr_model, sizeof(hdr_model),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        "%s\r\n",
        STT_BOUNDARY, model);

    char hdr_fmt[256];
    int hdr_fmt_len = snprintf(hdr_fmt, sizeof(hdr_fmt),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n"
        "\r\n"
        "json\r\n",
        STT_BOUNDARY);

    char trailer[64];
    int trailer_len = snprintf(trailer, sizeof(trailer),
        "--%s--\r\n", STT_BOUNDARY);

    *total_len = (size_t)hdr_file_len + audio_len + (size_t)hdr_model_len
               + (size_t)hdr_fmt_len + (size_t)trailer_len;

    uint8_t *body = ps_malloc(*total_len + 1);
    if (!body) return NULL;

    size_t off = 0;
    memcpy(body + off, hdr_file,  hdr_file_len);  off += hdr_file_len;
    memcpy(body + off, audio,     audio_len);      off += audio_len;
    memcpy(body + off, hdr_model, hdr_model_len);  off += hdr_model_len;
    memcpy(body + off, hdr_fmt,   hdr_fmt_len);    off += hdr_fmt_len;
    memcpy(body + off, trailer,   trailer_len);    off += trailer_len;
    body[off] = '\0';

    return body;
}

/* ── Local STT (plain HTTP, no TLS) ────────────────────────────── */

static bool local_stt_available(void)
{
    if (!s_local_url[0]) return false;
    if (s_local_offline) {
        if ((esp_timer_get_time() - s_local_fail_us) < LOCAL_BACKOFF_US) return false;
        s_local_offline = false;
    }
    return true;
}

static esp_err_t stt_transcribe_local(const uint8_t *audio, size_t audio_len,
                                       const char *mime, stt_result_t *out)
{
    char url[STT_LOCAL_URL_MAX + 48];
    snprintf(url, sizeof(url), "%s/v1/audio/transcriptions", s_local_url);

    const char *use_mime  = (mime && mime[0]) ? mime : "audio/webm;codecs=opus";
    const char *use_model = s_model[0] ? s_model : "whisper-large-v3-turbo";

    size_t body_len = 0;
    uint8_t *body = build_multipart(audio, audio_len, use_mime, use_model, &body_len);
    if (!body) return ESP_ERR_NO_MEM;

    resp_buf_t rb = { .data = ps_malloc(2048), .len = 0, .cap = 2048, .oom = false };
    if (!rb.data) { free(body); return ESP_ERR_NO_MEM; }
    rb.data[0] = '\0';

    char ct[64];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", STT_BOUNDARY);

    esp_http_client_config_t cfg = {
        .url            = url,
        .event_handler  = http_event_cb,
        .user_data      = &rb,
        .timeout_ms     = 10000,  /* local LAN — generous for transcription */
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); free(rb.data); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ct);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    ESP_LOGI(TAG, "STT local: POST %s (%u audio bytes)", url, (unsigned)audio_len);
    ws_server_broadcast_monitor_verbose("stt", "Trying local STT (mlx-audio)...");

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (ret != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Local STT failed: %s (HTTP %d)", esp_err_to_name(ret), status);
        s_local_offline = true;
        s_local_fail_us = esp_timer_get_time();
        ws_server_broadcast_monitor_verbose("stt", "Local STT offline, falling back to cloud");
        free(rb.data);
        return ESP_FAIL;
    }

    /* Parse JSON — may be NDJSON (mlx-audio streams); take last complete JSON */
    char *json_start = rb.data;
    /* If NDJSON, find the last line with "text" */
    char *last_text_line = NULL;
    char *line = rb.data;
    while (*line) {
        if (strstr(line, "\"text\"")) last_text_line = line;
        char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    if (last_text_line) json_start = last_text_line;

    cJSON *root = cJSON_Parse(json_start);
    free(rb.data);

    if (!root) {
        strncpy(out->error, "Invalid JSON from local STT", sizeof(out->error) - 1);
        s_local_offline = true;
        s_local_fail_us = esp_timer_get_time();
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring[0]) {
        strncpy(out->text, text_item->valuestring, sizeof(out->text) - 1);
        ESP_LOGI(TAG, "Local STT: \"%s\"", out->text);
        ws_server_broadcast_monitor_verbose("stt", "Local STT success");
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_Delete(root);
    strncpy(out->error, "Empty local STT result", sizeof(out->error) - 1);
    return ESP_FAIL;
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t stt_client_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_STT, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[STT_API_KEY_MAX];
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, LANG_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        }
        char etmp[STT_ENDPOINT_MAX];
        len = sizeof(etmp);
        if (nvs_get_str(nvs, "endpoint", etmp, &len) == ESP_OK && etmp[0]) {
            strncpy(s_endpoint, etmp, sizeof(s_endpoint) - 1);
        }
        char mtmp[STT_MODEL_MAX];
        len = sizeof(mtmp);
        if (nvs_get_str(nvs, "model", mtmp, &len) == ESP_OK && mtmp[0]) {
            strncpy(s_model, mtmp, sizeof(s_model) - 1);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "STT client ready (model: %s)", s_model);
    } else {
        ESP_LOGW(TAG, "No STT API key. Use CLI: stt_key <KEY>");
    }

    /* Create persistent session for TLS reuse */
    if (!s_session.valid) {
        esp_err_t err = http_session_init(&s_session, s_endpoint,
                                          http_event_cb,
                                          60 * 1000, 4096, 4096);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STT http_session_init failed: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t stt_transcribe(const uint8_t *audio, size_t audio_len,
                          const char *mime, stt_result_t *out)
{
    if (!audio || audio_len == 0 || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* Try local STT first */
    if (local_stt_available()) {
        esp_err_t local_ret = stt_transcribe_local(audio, audio_len, mime, out);
        if (local_ret == ESP_OK && out->text[0]) {
            return ESP_OK;
        }
        /* Clear error from local attempt before cloud fallback */
        memset(out, 0, sizeof(*out));
    }

    if (!s_api_key[0]) {
        strncpy(out->error, "No STT API key configured and local STT unavailable",
                sizeof(out->error) - 1);
        return ESP_ERR_INVALID_STATE;
    }

    const char *use_mime  = (mime && mime[0]) ? mime : "audio/webm;codecs=opus";
    const char *use_model = s_model[0] ? s_model : LANG_DEFAULT_STT_MODEL;

    /* Build multipart body in PSRAM */
    size_t body_len = 0;
    uint8_t *body = build_multipart(audio, audio_len, use_mime, use_model, &body_len);
    if (!body) {
        strncpy(out->error, "Out of PSRAM for multipart body", sizeof(out->error) - 1);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "STT POST: %u audio bytes → %u multipart body", (unsigned)audio_len, (unsigned)body_len);
    {
        char stt_info[80];
        snprintf(stt_info, sizeof(stt_info), "Sending %.1fs audio (%uKB) to Groq Whisper",
                 (float)audio_len / (LANG_MIC_SAMPLE_RATE * 2), (unsigned)(audio_len / 1024));
        ws_server_broadcast_monitor_verbose("stt", stt_info);
    }

    /* Response buffer — small initial alloc (JSON response is tiny) */
    resp_buf_t rb = { .data = ps_malloc(2048), .len = 0, .cap = 2048, .oom = false };
    if (!rb.data) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    rb.data[0] = '\0';

    /* Build Content-Type header with boundary */
    char ct[64];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", STT_BOUNDARY);

    char auth[STT_API_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);

    /* Reuse persistent session (lazy init fallback if init was skipped) */
    if (!s_session.valid) {
        http_session_init(&s_session, s_endpoint, http_event_cb,
                          60 * 1000, 4096, 4096);
    }
    if (!s_session.valid) {
        free(body);
        free(rb.data);
        return ESP_FAIL;
    }

    /* Point session at this call's response buffer and configure request */
    http_session_set_ctx(&s_session, &rb);

    esp_http_client_set_method(s_session.handle, HTTP_METHOD_POST);
    esp_http_client_set_header(s_session.handle, "Authorization", auth);
    esp_http_client_set_header(s_session.handle, "Content-Type", ct);
    esp_http_client_set_post_field(s_session.handle, (const char *)body, (int)body_len);

    esp_err_t ret = esp_http_client_perform(s_session.handle);

    /* If connection was stale, reset session and retry with headers re-applied.
     * http_session_reset() creates a fresh handle — old headers/body are lost. */
    if (ret == ESP_ERR_HTTP_CONNECT || ret == ESP_ERR_HTTP_CONNECTION_CLOSED) {
        ESP_LOGW(TAG, "STT connection lost (%s), resetting and retrying", esp_err_to_name(ret));
        ws_server_broadcast_monitor_verbose("stt", "Connection lost, retrying...");
        http_session_reset(&s_session);
        if (s_session.valid) {
            /* Re-apply all request params on the fresh handle */
            http_session_set_ctx(&s_session, &rb);
            esp_http_client_set_method(s_session.handle, HTTP_METHOD_POST);
            esp_http_client_set_header(s_session.handle, "Authorization", auth);
            esp_http_client_set_header(s_session.handle, "Content-Type", ct);
            esp_http_client_set_post_field(s_session.handle, (const char *)body, (int)body_len);
            rb.len = 0;  /* reset response buffer for retry */
            if (rb.data) rb.data[0] = '\0';
            ret = esp_http_client_perform(s_session.handle);
        }
    }

    int status = esp_http_client_get_status_code(s_session.handle);
    /* Do NOT cleanup — keep handle alive for TLS session reuse */
    free(body);

    if (ret != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "HTTP error: %s", esp_err_to_name(ret));
        free(rb.data);
        return ret;
    }

    /* Check for OOM during response accumulation */
    if (rb.oom) {
        ESP_LOGE(TAG, "STT response buffer OOM");
        free(rb.data);
        strncpy(out->error, "Out of memory accumulating STT response", sizeof(out->error) - 1);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "STT HTTP %d, response: %u bytes", status, (unsigned)rb.len);

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    free(rb.data);

    if (!root) {
        strncpy(out->error, "Invalid JSON response from STT", sizeof(out->error) - 1);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;
    if (status == 200) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item) && text_item->valuestring[0]) {
            strncpy(out->text, text_item->valuestring, sizeof(out->text) - 1);
            result = ESP_OK;
            /* Verbose log: show what Groq heard */
            {
                char stt_out[160];
                snprintf(stt_out, sizeof(stt_out), "Transcribed: \"%.140s\"",
                         text_item->valuestring);
                ws_server_broadcast_monitor_verbose("stt", stt_out);
            }
        } else {
            strncpy(out->error, "Empty transcription result", sizeof(out->error) - 1);
        }
    } else {
        /* Extract error message */
        cJSON *err  = cJSON_GetObjectItem(root, "error");
        cJSON *msg  = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (cJSON_IsString(msg) && msg->valuestring[0]) {
            snprintf(out->error, sizeof(out->error), "STT API %d: %s", status, msg->valuestring);
        } else {
            snprintf(out->error, sizeof(out->error), "STT API error: HTTP %d", status);
        }
    }

    cJSON_Delete(root);
    return result;
}

void stt_set_api_key(const char *key)
{
    if (!key) return;
    strncpy(s_api_key, key, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_STT, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, LANG_NVS_KEY_API_KEY, key);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "STT API key saved to NVS");
    }
}

void stt_get_api_key_masked(char *buf, size_t buf_len)
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

void stt_set_local_url(const char *url)
{
    if (!url) { s_local_url[0] = '\0'; return; }
    strncpy(s_local_url, url, sizeof(s_local_url) - 1);
    s_local_url[sizeof(s_local_url) - 1] = '\0';
    /* Strip trailing slash */
    size_t len = strlen(s_local_url);
    if (len > 0 && s_local_url[len - 1] == '/') s_local_url[len - 1] = '\0';
    s_local_offline = false;
    ESP_LOGI(TAG, "Local STT URL: %s", s_local_url);
}

const char *stt_get_local_url(void)
{
    return s_local_url;
}
