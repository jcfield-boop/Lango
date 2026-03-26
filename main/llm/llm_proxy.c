#include "llm_proxy.h"
#include "langoustine_config.h"
#include "proxy/http_proxy.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Forward declaration — warmup task defined near end of file */
static void llm_warmup_task(void *arg);

static const char *TAG = "llm";

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_DUMP_MAX_BYTES   (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = MIMI_LLM_DEFAULT_MODEL;
static char s_provider[16] = MIMI_LLM_PROVIDER_DEFAULT;

/* Local/Ollama model: full base URL e.g. "http://192.168.0.25:11434/v1" */
#define LLM_LOCAL_URL_MAX 128
static char s_local_url[LLM_LOCAL_URL_MAX] = {0};
static char s_local_model[LLM_MODEL_MAX_LEN] = {0};

/* Text-only local model (e.g. gemma3:12b) — used for non-vision turns.
 * Falls back to s_local_model when not set. */
static char s_local_text_model[LLM_MODEL_MAX_LEN] = {0};

/* Voice-channel routing: when chat_id=="ptt", override to a fast cloud model
 * rather than local Ollama (which is too slow for real-time voice interaction).
 * Configured via SERVICES.md: voice_provider / voice_model under [Local Model]. */
static char s_voice_provider[16] = {0};
static char s_voice_model[LLM_MODEL_MAX_LEN] = {0};

/* Per-request override (single agent task, no concurrency) */
static char s_override_provider[16] = {0};
static char s_override_model[LLM_MODEL_MAX_LEN] = {0};
static bool s_override_active = false;

/* Cached local health check */
static bool s_local_online = false;
static int64_t s_local_check_us = 0;
#define LOCAL_HEALTH_CACHE_US  (15LL * 1000000LL)  /* 15 seconds — short enough to catch model eviction */

static uint32_t s_total_input_tokens    = 0;
static uint32_t s_total_output_tokens   = 0;
static uint32_t s_total_cost_millicents = 0;

static void llm_log_payload(const char *label, const char *payload)
{
    if (!payload) {
        ESP_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if MIMI_LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    ESP_LOGI(TAG, "%s (%u bytes)%s",
             label,
             (unsigned)total,
             (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for (size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES) {
        size_t n = shown - off;
        if (n > LLM_DUMP_CHUNK_BYTES) {
            n = LLM_DUMP_CHUNK_BYTES;
        }
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        ESP_LOGI(TAG, "%s[%u]: %s", label, (unsigned)off, chunk);
    }
#else
    if (MIMI_LLM_LOG_PREVIEW_BYTES > 0) {
        size_t shown = total > MIMI_LLM_LOG_PREVIEW_BYTES ? MIMI_LLM_LOG_PREVIEW_BYTES : total;
        char preview[MIMI_LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for (size_t i = 0; i < shown; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "%s (%u bytes): %s%s",
                 label,
                 (unsigned)total,
                 preview,
                 (shown < total) ? " ..." : "");
    } else {
        ESP_LOGI(TAG, "%s (%u bytes)", label, (unsigned)total);
    }
#endif
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = ps_calloc(1, initial_cap);  /* Allocate from PSRAM — large buffer for S3 */
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = ps_realloc(rb->data, new_cap);  /* Keep in PSRAM */
        if (!tmp) {
            char emsg[80];
            snprintf(emsg, sizeof(emsg), "LLM: resp buf OOM at cap %u bytes", (unsigned)rb->cap);
            ws_server_broadcast_monitor_verbose("error", emsg);
            return ESP_ERR_NO_MEM;
        }
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

/* ── HTTP event handler (for esp_http_client direct path) ─────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_buf_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* ── Provider helpers ──────────────────────────────────────────── */

static const char *effective_provider(void)
{
    return s_override_active ? s_override_provider : s_provider;
}

static const char *effective_model(void)
{
    return s_override_active ? s_override_model : s_model;
}

static bool provider_is_openai(void)
{
    return strcmp(effective_provider(), "openai") == 0;
}

static bool provider_is_openrouter(void)
{
    return strcmp(effective_provider(), "openrouter") == 0;
}

static bool provider_is_ollama(void)
{
    return strcmp(effective_provider(), "ollama") == 0;
}

static bool provider_is_ollama(void)
{
    return strcmp(s_provider, "ollama") == 0;
}

/* Returns true for any provider that uses OpenAI-compatible message/tool format */
static bool provider_uses_openai_format(void)
{
    return provider_is_openai() || provider_is_openrouter() || provider_is_ollama();
}

/* Returns true when the target is plain HTTP (no TLS) — e.g. local Ollama */
static bool provider_is_plaintext(void)
{
    if (provider_is_ollama() && s_local_url[0]) {
        return strncmp(s_local_url, "http://", 7) == 0;
    }
    return false;
}

static const char *llm_api_url(void)
{
    if (provider_is_ollama() && s_local_url[0]) {
        /* s_local_url already contains full URL like "http://192.168.0.25:11434/v1" */
        static char url_buf[LLM_LOCAL_URL_MAX + 32];
        snprintf(url_buf, sizeof(url_buf), "%s/chat/completions", s_local_url);
        return url_buf;
    }
    if (provider_is_openrouter()) return MIMI_OPENROUTER_API_URL;
    if (provider_is_openai())     return MIMI_OPENAI_API_URL;
    return MIMI_LLM_API_URL;
}

static const char *llm_api_host(void)
{
    if (provider_is_ollama() && s_local_url[0]) {
        /* Extract host from URL like "http://192.168.0.25:11434/v1" */
        static char host_buf[64];
        const char *p = s_local_url;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        else if (strncmp(p, "https://", 8) == 0) p += 8;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
        memcpy(host_buf, p, len);
        host_buf[len] = '\0';
        return host_buf;
    }
    if (provider_is_openrouter()) return "openrouter.ai";
    if (provider_is_openai())     return "api.openai.com";
    return "api.anthropic.com";
}

static const char *llm_api_path(void)
{
    if (provider_is_ollama()) return "/v1/chat/completions";
    if (provider_is_openrouter()) return "/api/v1/chat/completions";
    if (provider_is_openai())     return "/v1/chat/completions";
    return "/v1/messages";
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t llm_proxy_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }
    if (MIMI_SECRET_MODEL[0] != '\0') {
        safe_copy(s_model, sizeof(s_model), MIMI_SECRET_MODEL);
    }
    if (MIMI_SECRET_MODEL_PROVIDER[0] != '\0') {
        safe_copy(s_provider, sizeof(s_provider), MIMI_SECRET_MODEL_PROVIDER);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[LLM_API_KEY_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp);
        }
        char model_tmp[LLM_MODEL_MAX_LEN] = {0};
        len = sizeof(model_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, model_tmp, &len) == ESP_OK && model_tmp[0]) {
            safe_copy(s_model, sizeof(s_model), model_tmp);
        }
        char provider_tmp[16] = {0};
        len = sizeof(provider_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_PROVIDER, provider_tmp, &len) == ESP_OK && provider_tmp[0]) {
            safe_copy(s_provider, sizeof(s_provider), provider_tmp);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "LLM proxy initialized (provider: %s, model: %s)", s_provider, s_model);
    } else {
        ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
    }

    /* Pre-warm local Ollama model so the first request doesn't pay cold-load cost.
     * Runs in a background task (12s delay) to avoid blocking app_main startup. */
    if (s_local_url[0] && s_local_model[0]) {
        ESP_LOGI(TAG, "Local LLM configured (%s) — scheduling Ollama warmup", s_local_model);
        xTaskCreate(llm_warmup_task, "llm_warmup", 4 * 1024, NULL, 2, NULL);
    }

    return ESP_OK;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static esp_err_t llm_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
    esp_http_client_config_t config = {
        .url = llm_api_url(),
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = provider_is_plaintext() ? 240 * 1000 : 120 * 1000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = provider_is_plaintext() ? NULL : esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (provider_uses_openai_format()) {
        if (s_api_key[0]) {
            char auth[LLM_API_KEY_MAX_LEN + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
            esp_http_client_set_header(client, "Authorization", auth);
        }
        if (provider_is_openrouter()) {
            esp_http_client_set_header(client, "HTTP-Referer", MIMI_OPENROUTER_REFERER);
            esp_http_client_set_header(client, "X-Title",      MIMI_OPENROUTER_TITLE);
        }
    } else {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", MIMI_LLM_API_VERSION);
    }
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static esp_err_t llm_http_via_proxy(const char *post_data, resp_buf_t *rb, int *out_status)
{
    proxy_conn_t *conn = proxy_conn_open(llm_api_host(), 443, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_data);
    char header[1024];
    int hlen = 0;
    if (provider_is_openrouter()) {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "HTTP-Referer: %s\r\n"
            "X-Title: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            llm_api_path(), llm_api_host(), s_api_key,
            MIMI_OPENROUTER_REFERER, MIMI_OPENROUTER_TITLE, body_len);
    } else if (provider_is_openai()) {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            llm_api_path(), llm_api_host(), s_api_key, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            llm_api_path(), llm_api_host(), s_api_key, MIMI_LLM_API_VERSION, body_len);
    }

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response into buffer */
    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0) break;
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
    }
    proxy_conn_close(conn);

    /* Parse status line */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
    }

    /* Strip HTTP headers, keep body only */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    return ESP_OK;
}

/* ── Shared HTTP dispatch ─────────────────────────────────────── */

static esp_err_t llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        return llm_http_via_proxy(post_data, rb, out_status);
    } else {
        return llm_http_direct(post_data, rb, out_status);
    }
}

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }

        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");

            /* collect text */
            char *text_buf = NULL;
            size_t off = 0;
            cJSON *block;
            cJSON *tool_calls = NULL;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = ps_realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                    }
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;

                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) {
                        char *args = cJSON_PrintUnformatted(input);
                        if (args) {
                            cJSON_AddStringToObject(func, "arguments", args);
                            free(args);
                        }
                    }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            if (text_buf) {
                cJSON_AddStringToObject(m, "content", text_buf);
            } else {
                cJSON_AddStringToObject(m, "content", "");
            }
            if (tool_calls) {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(out, m);
            free(text_buf);
        } else if (strcmp(role->valuestring, "user") == 0) {
            /* tool_result blocks become role=tool */
            cJSON *block;
            bool has_user_text = false;
            char *text_buf = NULL;
            size_t off = 0;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = ps_realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                        has_user_text = true;
                    }
                }
            }
            if (has_user_text) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }

    return out;
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         bool force_tool_use,
                         llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0' && !provider_is_ollama()) return ESP_ERR_INVALID_STATE;

    {
        char llm_info[96];
        snprintf(llm_info, sizeof(llm_info), "provider=%s model=%s", effective_provider(), effective_model());
        ws_server_broadcast_monitor("llm", llm_info);
    }

    /* Build request body (non-streaming) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", effective_model());
    if (provider_is_openai()) {
        /* OpenAI uses the newer max_completion_tokens parameter */
        cJSON_AddNumberToObject(body, "max_completion_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        /* Anthropic and OpenRouter both use max_tokens */
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }

    if (provider_uses_openai_format()) {
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);

        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                /* "required" forces at least one tool call; "auto" lets model skip */
                cJSON_AddStringToObject(body, "tool_choice",
                                        force_tool_use ? "required" : "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt);

        /* Deep-copy messages so caller keeps ownership */
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);

        /* Add tools array if provided */
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                /* Anthropic: tool_choice object; "any" = must call at least one tool */
                if (force_tool_use) {
                    cJSON *tc = cJSON_CreateObject();
                    cJSON_AddStringToObject(tc, "type", "any");
                    cJSON_AddItemToObject(body, "tool_choice", tc);
                }
                /* Without tool_choice, Anthropic defaults to auto */
            }
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling LLM API with tools (provider: %s, model: %s, body: %d bytes)",
             effective_provider(), effective_model(), (int)strlen(post_data));
    llm_log_payload("LLM tools request", post_data);

    /* HTTP call */
    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = llm_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        char emsg[80];
        snprintf(emsg, sizeof(emsg), "LLM HTTP failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", emsg);
        ws_server_broadcast_monitor("error", emsg);
        llm_log_payload("LLM tools partial response", rb.data);
        resp_buf_free(&rb);
        return err;
    }

    llm_log_payload("LLM tools raw response", rb.data);
    {
        char szlog[64];
        snprintf(szlog, sizeof(szlog), "LLM raw resp: %d bytes", (int)rb.len);
        ws_server_broadcast_monitor("llm", szlog);
    }

    if (status != 200) {
        /* Broadcast the first ~120 chars of the error body (strip control chars) */
        char emsg[160];
        snprintf(emsg, sizeof(emsg), "LLM API error %d: %.110s",
                 status, rb.data ? rb.data : "");
        for (char *p = emsg; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        ESP_LOGE(TAG, "%s", emsg);
        ws_server_broadcast_monitor("error", emsg);
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse full JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse API response JSON");
        ws_server_broadcast_monitor("error", "LLM: failed to parse API response (truncated?)");
        return ESP_FAIL;
    }

    /* Broadcast which model was actually used — useful when using openrouter/auto */
    {
        cJSON *model_item = cJSON_GetObjectItem(root, "model");
        if (model_item && cJSON_IsString(model_item)) {
            char mmsg[80];
            snprintf(mmsg, sizeof(mmsg), "LLM model: %s", model_item->valuestring);
            ws_server_broadcast_monitor("llm", mmsg);
        }
    }

    if (provider_uses_openai_format()) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        if (choice0) {
            cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
            if (finish && cJSON_IsString(finish)) {
                resp->tool_use  = (strcmp(finish->valuestring, "tool_calls") == 0);
                resp->truncated = (strcmp(finish->valuestring, "length") == 0);
                char fmsg[64];
                snprintf(fmsg, sizeof(fmsg), "LLM finish_reason: %s", finish->valuestring);
                ws_server_broadcast_monitor_verbose("llm", fmsg);
            }

            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (!content || cJSON_IsNull(content)) {
                    /* content:null is expected when finish_reason=tool_calls;
                     * unexpected when finish_reason=stop — log it either way */
                    char emsg[80];
                    snprintf(emsg, sizeof(emsg), "LLM: null content (finish=%s)",
                             finish && cJSON_IsString(finish) ? finish->valuestring : "?");
                    ws_server_broadcast_monitor_verbose("llm", emsg);
                } else if (!cJSON_IsString(content)) {
                    char emsg[64];
                    snprintf(emsg, sizeof(emsg), "LLM: unexpected content type %d", content->type);
                    ws_server_broadcast_monitor("error", emsg);
                } else {
                    size_t tlen = strlen(content->valuestring);
                    {
                        char vmsg[80];
                        snprintf(vmsg, sizeof(vmsg), "LLM text alloc: %u bytes (heap: %u free)",
                                 (unsigned)tlen,
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                        ws_server_broadcast_monitor_verbose("llm", vmsg);
                    }
                    resp->text = ps_calloc(1, tlen + 1);  /* PSRAM: keep out of SRAM */
                    if (!resp->text) {
                        char emsg[96];
                        snprintf(emsg, sizeof(emsg), "LLM: OOM for text (%u bytes, heap %u free)",
                                 (unsigned)tlen,
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                        ESP_LOGE(TAG, "%s", emsg);
                        ws_server_broadcast_monitor("error", emsg);
                        cJSON_Delete(root);
                        return ESP_ERR_NO_MEM;
                    }
                    memcpy(resp->text, content->valuestring, tlen);
                    resp->text_len = tlen;
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        cJSON *func = cJSON_GetObjectItem(tc, "function");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        if (func) {
                            cJSON *name = cJSON_GetObjectItem(func, "name");
                            cJSON *args = cJSON_GetObjectItem(func, "arguments");
                            if (name && cJSON_IsString(name)) {
                                strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                            }
                            if (args && cJSON_IsString(args)) {
                                call->input = strdup(args->valuestring);
                                if (call->input) {
                                    call->input_len = strlen(call->input);
                                }
                            }
                        }
                        resp->call_count++;
                    }
                    if (resp->call_count > 0) {
                        resp->tool_use = true;
                    }
                }
            }
        }

        /* Parse usage (OpenAI/OpenRouter format) */
        cJSON *usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
            if (pt && cJSON_IsNumber(pt)) resp->input_tokens  = (uint32_t)pt->valueint;
            if (ct && cJSON_IsNumber(ct)) resp->output_tokens = (uint32_t)ct->valueint;
            if (provider_is_openrouter()) {
                cJSON *cost = cJSON_GetObjectItem(usage, "cost");
                if (cost && cJSON_IsNumber(cost)) {
                    s_total_cost_millicents += (uint32_t)(cost->valuedouble * 100000.0 + 0.5);
                }
            }
            s_total_input_tokens  += resp->input_tokens;
            s_total_output_tokens += resp->output_tokens;
        }
    } else {
        /* stop_reason */
        cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
        if (stop_reason && cJSON_IsString(stop_reason)) {
            resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
        }

        /* Iterate content blocks */
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsArray(content)) {
            /* Accumulate total text length first */
            size_t total_text = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        total_text += strlen(text->valuestring);
                    }
                }
            }

            /* Allocate and copy text */
            if (total_text > 0) {
                {
                    char vmsg[80];
                    snprintf(vmsg, sizeof(vmsg), "LLM text alloc: %u bytes (heap: %u free)",
                             (unsigned)total_text,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    ws_server_broadcast_monitor_verbose("llm", vmsg);
                }
                resp->text = ps_calloc(1, total_text + 1);  /* PSRAM */
                if (!resp->text) {
                    char emsg[96];
                    snprintf(emsg, sizeof(emsg), "LLM: OOM for text (%u bytes, heap %u free)",
                             (unsigned)total_text,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    ESP_LOGE(TAG, "%s", emsg);
                    ws_server_broadcast_monitor("error", emsg);
                    cJSON_Delete(root);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_ArrayForEach(block, content) {
                    cJSON *btype = cJSON_GetObjectItem(block, "type");
                    if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (!text || !cJSON_IsString(text)) continue;
                    size_t tlen = strlen(text->valuestring);
                    memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                    resp->text_len += tlen;
                }
                resp->text[resp->text_len] = '\0';
            }

            /* Extract tool_use blocks */
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
                if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;

                llm_tool_call_t *call = &resp->calls[resp->call_count];

                cJSON *id = cJSON_GetObjectItem(block, "id");
                if (id && cJSON_IsString(id)) {
                    strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                }

                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    /* cJSON_PrintUnformatted allocates a new string not owned by the
                     * cJSON tree, so cJSON_Delete(root) below will NOT free it.
                     * Ownership transfers to call->input; freed by llm_response_free(). */
                    char *input_str = cJSON_PrintUnformatted(input);
                    call->input     = input_str;   /* NULL on OOM — safe for free() */
                    call->input_len = input_str ? strlen(input_str) : 0;
                }

                resp->call_count++;
            }
        }

        /* Parse usage (Anthropic format) */
        cJSON *usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
            cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
            if (it && cJSON_IsNumber(it)) resp->input_tokens  = (uint32_t)it->valueint;
            if (ot && cJSON_IsNumber(ot)) resp->output_tokens = (uint32_t)ot->valueint;
            s_total_input_tokens  += resp->input_tokens;
            s_total_output_tokens += resp->output_tokens;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_use" : "end_turn");

    {
        char summary[96];
        snprintf(summary, sizeof(summary), "LLM: %lu\u2191 %lu\u2193 tok, %d tool%s, stop=%s",
                 (unsigned long)resp->input_tokens, (unsigned long)resp->output_tokens,
                 resp->call_count, resp->call_count == 1 ? "" : "s",
                 resp->tool_use ? "tool_use" : "end_turn");
        ws_server_broadcast_monitor("llm", summary);
    }

    return ESP_OK;
}

/* ── SSE Streaming ────────────────────────────────────────────── */

#define SSE_LINE_MAX   2048  /* large enough for tool-call arg chunks */
#define SSE_TC_MAX     MIMI_MAX_TOOL_CALLS

/* Rate-limit thresholds for progress callbacks */
#define SSE_PROGRESS_MIN_CHARS  8
#define SSE_PROGRESS_MIN_US     150000LL

typedef struct {
    char        id[64];
    char        name[32];
    resp_buf_t  args;       /* lazily initialized on first fragment */
} sse_tc_t;

typedef struct {
    /* Line accumulator */
    char        line[SSE_LINE_MAX + 1];
    int         line_len;

    /* Accumulated response */
    resp_buf_t  text;
    sse_tc_t    tc[SSE_TC_MAX];
    int         tc_count;

    /* Stop conditions */
    bool        tool_use;
    bool        truncated;
    bool        done;

    /* Token usage */
    uint32_t    input_tokens;
    uint32_t    output_tokens;

    /* Anthropic block tracking */
    int         cur_block_index;
    char        cur_block_type[16];

    /* Progress callback */
    llm_stream_progress_fn progress_cb;
    void       *progress_ctx;
    size_t      last_progress_len;
    int64_t     last_progress_us;

    /* Hard total-stream deadline (esp_timer_get_time() units, µs).
     * Set before esp_http_client_perform(); checked in the data handler.
     * Returning ESP_FAIL from the handler aborts the HTTP client cleanly. */
    int64_t     stream_deadline_us;
} sse_state_t;

static sse_state_t *sse_state_alloc(void)
{
    /* Use PSRAM: the struct contains a 2KB line buffer + tc[4] arg buffers.
     * Keeping it out of SRAM avoids ~3 KB drop on every LLM call, which
     * previously pushed heap_min below the 20 KB panic floor. */
    sse_state_t *st = ps_calloc(1, sizeof(sse_state_t));
    if (!st) return NULL;
    if (resp_buf_init(&st->text, 2048) != ESP_OK) {
        free(st);
        return NULL;
    }
    st->last_progress_us = esp_timer_get_time();
    return st;
}

static void sse_state_free(sse_state_t *st)
{
    if (!st) return;
    resp_buf_free(&st->text);
    for (int i = 0; i < SSE_TC_MAX; i++) {
        resp_buf_free(&st->tc[i].args);
    }
    free(st);
}

static void sse_maybe_emit_progress(sse_state_t *st)
{
    if (!st->progress_cb || !st->text.data) return;
    size_t cur_len = st->text.len;
    if (cur_len <= st->last_progress_len) return;

    size_t new_chars = cur_len - st->last_progress_len;
    int64_t now = esp_timer_get_time();
    if (new_chars < SSE_PROGRESS_MIN_CHARS &&
        (now - st->last_progress_us) < SSE_PROGRESS_MIN_US) return;

    st->progress_cb(st->text.data, cur_len, st->progress_ctx);
    st->last_progress_len = cur_len;
    st->last_progress_us  = now;
}

/* ── OpenAI / OpenRouter SSE chunk ─────────────────────────────── */

static void sse_process_openai(sse_state_t *st, cJSON *root)
{
    /* Top-level usage (stream_options: include_usage sends it in last chunk) */
    cJSON *usage_top = cJSON_GetObjectItem(root, "usage");
    if (usage_top) {
        cJSON *pt = cJSON_GetObjectItem(usage_top, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage_top, "completion_tokens");
        if (pt && cJSON_IsNumber(pt)) st->input_tokens  = (uint32_t)pt->valueint;
        if (ct && cJSON_IsNumber(ct)) st->output_tokens = (uint32_t)ct->valueint;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *c0 = (choices && cJSON_IsArray(choices))
                    ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (!c0) return;

    /* finish_reason */
    cJSON *finish = cJSON_GetObjectItem(c0, "finish_reason");
    if (finish && cJSON_IsString(finish) && finish->valuestring[0]) {
        if (strcmp(finish->valuestring, "tool_calls") == 0) st->tool_use  = true;
        if (strcmp(finish->valuestring, "length")     == 0) st->truncated = true;
        st->done = true;
    }

    cJSON *delta = cJSON_GetObjectItem(c0, "delta");
    if (!delta) return;

    /* Text content */
    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        resp_buf_append(&st->text, content->valuestring, strlen(content->valuestring));
    }

    /* Tool calls */
    cJSON *tc_arr = cJSON_GetObjectItem(delta, "tool_calls");
    if (!tc_arr || !cJSON_IsArray(tc_arr)) return;

    cJSON *tc_item;
    cJSON_ArrayForEach(tc_item, tc_arr) {
        cJSON *idx_j = cJSON_GetObjectItem(tc_item, "index");
        int idx = (idx_j && cJSON_IsNumber(idx_j)) ? idx_j->valueint : 0;
        if (idx < 0 || idx >= SSE_TC_MAX) continue;
        if (idx >= st->tc_count) st->tc_count = idx + 1;

        cJSON *id = cJSON_GetObjectItem(tc_item, "id");
        if (id && cJSON_IsString(id) && id->valuestring[0]) {
            strncpy(st->tc[idx].id, id->valuestring, sizeof(st->tc[idx].id) - 1);
        }

        cJSON *func = cJSON_GetObjectItem(tc_item, "function");
        if (!func) continue;

        cJSON *name = cJSON_GetObjectItem(func, "name");
        if (name && cJSON_IsString(name) && name->valuestring[0]) {
            strncpy(st->tc[idx].name, name->valuestring, sizeof(st->tc[idx].name) - 1);
        }

        cJSON *args = cJSON_GetObjectItem(func, "arguments");
        if (args && cJSON_IsString(args)) {
            if (!st->tc[idx].args.data) {
                resp_buf_init(&st->tc[idx].args, 256);
            }
            resp_buf_append(&st->tc[idx].args, args->valuestring, strlen(args->valuestring));
        }
    }
}

/* ── Anthropic SSE chunk ────────────────────────────────────────── */

static void sse_process_anthropic(sse_state_t *st, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) return;
    const char *t = type->valuestring;

    if (strcmp(t, "message_start") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (msg) {
            cJSON *usage = cJSON_GetObjectItem(msg, "usage");
            if (usage) {
                cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
                if (it && cJSON_IsNumber(it)) st->input_tokens = (uint32_t)it->valueint;
            }
        }
    } else if (strcmp(t, "content_block_start") == 0) {
        cJSON *idx_j = cJSON_GetObjectItem(root, "index");
        st->cur_block_index = (idx_j && cJSON_IsNumber(idx_j)) ? idx_j->valueint : 0;
        cJSON *cb = cJSON_GetObjectItem(root, "content_block");
        if (cb) {
            cJSON *btype = cJSON_GetObjectItem(cb, "type");
            if (btype && cJSON_IsString(btype)) {
                strncpy(st->cur_block_type, btype->valuestring,
                        sizeof(st->cur_block_type) - 1);
            }
            if (strcmp(st->cur_block_type, "tool_use") == 0) {
                int idx = st->cur_block_index;
                if (idx >= 0 && idx < SSE_TC_MAX) {
                    if (idx >= st->tc_count) st->tc_count = idx + 1;
                    cJSON *id   = cJSON_GetObjectItem(cb, "id");
                    cJSON *name = cJSON_GetObjectItem(cb, "name");
                    if (id   && cJSON_IsString(id))
                        strncpy(st->tc[idx].id, id->valuestring,
                                sizeof(st->tc[idx].id) - 1);
                    if (name && cJSON_IsString(name))
                        strncpy(st->tc[idx].name, name->valuestring,
                                sizeof(st->tc[idx].name) - 1);
                }
            }
        }
    } else if (strcmp(t, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (!delta) return;
        cJSON *dtype = cJSON_GetObjectItem(delta, "type");
        if (!dtype || !cJSON_IsString(dtype)) return;

        if (strcmp(dtype->valuestring, "text_delta") == 0) {
            cJSON *text = cJSON_GetObjectItem(delta, "text");
            if (text && cJSON_IsString(text)) {
                resp_buf_append(&st->text, text->valuestring, strlen(text->valuestring));
            }
        } else if (strcmp(dtype->valuestring, "input_json_delta") == 0) {
            int idx = st->cur_block_index;
            if (idx >= 0 && idx < SSE_TC_MAX) {
                cJSON *pj = cJSON_GetObjectItem(delta, "partial_json");
                if (pj && cJSON_IsString(pj)) {
                    if (!st->tc[idx].args.data) {
                        resp_buf_init(&st->tc[idx].args, 256);
                    }
                    resp_buf_append(&st->tc[idx].args, pj->valuestring,
                                    strlen(pj->valuestring));
                }
            }
        }
    } else if (strcmp(t, "message_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta) {
            cJSON *stop = cJSON_GetObjectItem(delta, "stop_reason");
            if (stop && cJSON_IsString(stop)) {
                if (strcmp(stop->valuestring, "tool_use")   == 0) st->tool_use  = true;
                if (strcmp(stop->valuestring, "max_tokens") == 0) st->truncated = true;
            }
        }
        cJSON *usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
            if (ot && cJSON_IsNumber(ot)) st->output_tokens = (uint32_t)ot->valueint;
        }
    } else if (strcmp(t, "message_stop") == 0) {
        st->done = true;
    }
}

/* ── SSE line dispatcher ─────────────────────────────────────────── */

static void sse_process_line(sse_state_t *st, const char *line, int len)
{
    if (len == 0) return;

    /* Skip "event: ..." lines */
    if (strncmp(line, "event: ", 7) == 0) return;

    /* Only handle "data: ..." lines */
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *data = line + 6;

    /* OpenAI sentinel */
    if (strcmp(data, "[DONE]") == 0) {
        st->done = true;
        return;
    }

    cJSON *root = cJSON_Parse(data);
    if (!root) return;

    if (provider_uses_openai_format()) {
        sse_process_openai(st, root);
    } else {
        sse_process_anthropic(st, root);
    }
    cJSON_Delete(root);

    sse_maybe_emit_progress(st);
}

/* ── SSE HTTP event handler ──────────────────────────────────────── */

static esp_err_t sse_http_event_handler(esp_http_client_event_t *evt)
{
    sse_state_t *st = (sse_state_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    /* Hard total-stream timeout: abort if we've been streaming too long.
     * The per-chunk timeout_ms in esp_http_client_config only covers the gap
     * between chunks, so a slow-but-steady model (e.g. qwen3-vl) can stream
     * for 800+ seconds without triggering it. This deadline catches that. */
    if (st->stream_deadline_us > 0 && esp_timer_get_time() > st->stream_deadline_us) {
        ESP_LOGW("llm", "LLM stream hard timeout — aborting");
        return ESP_FAIL;
    }

    const char *p = (const char *)evt->data;
    int remaining  = evt->data_len;

    while (remaining > 0) {
        /* Look for newline in this chunk */
        const char *nl = memchr(p, '\n', remaining);

        if (!nl) {
            /* No newline — accumulate into line buffer (cap at SSE_LINE_MAX) */
            int avail = SSE_LINE_MAX - st->line_len;
            int copy  = (remaining < avail) ? remaining : avail;
            if (copy > 0) {
                memcpy(st->line + st->line_len, p, copy);
                st->line_len += copy;
            }
            break;
        }

        /* Copy up to (but not including) the newline */
        int n     = (int)(nl - p);
        int avail = SSE_LINE_MAX - st->line_len;
        int copy  = (n < avail) ? n : avail;
        if (copy > 0) {
            memcpy(st->line + st->line_len, p, copy);
            st->line_len += copy;
        }

        /* Strip trailing \r */
        if (st->line_len > 0 && st->line[st->line_len - 1] == '\r') {
            st->line_len--;
        }
        st->line[st->line_len] = '\0';

        sse_process_line(st, st->line, st->line_len);
        st->line_len = 0;

        p         += n + 1;   /* skip past '\n' */
        remaining -= n + 1;
    }

    return ESP_OK;
}

/* ── Convert SSE state → llm_response_t ─────────────────────────── */

static esp_err_t sse_state_to_response(sse_state_t *st, llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (st->text.len > 0) {
        {
            char vmsg[80];
            snprintf(vmsg, sizeof(vmsg), "LLM text alloc: %u bytes (heap: %u free)",
                     (unsigned)st->text.len,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            ws_server_broadcast_monitor_verbose("llm", vmsg);
        }
        resp->text = ps_calloc(1, st->text.len + 1);  /* PSRAM: keeps response out of SRAM */
        if (!resp->text) {
            char emsg[96];
            snprintf(emsg, sizeof(emsg), "LLM: OOM for text (%u bytes, heap %u free)",
                     (unsigned)st->text.len,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            ESP_LOGE(TAG, "%s", emsg);
            ws_server_broadcast_monitor("error", emsg);
            return ESP_ERR_NO_MEM;
        }
        memcpy(resp->text, st->text.data, st->text.len);
        resp->text_len = st->text.len;
    }

    resp->tool_use      = st->tool_use;
    resp->truncated     = st->truncated;
    resp->input_tokens  = st->input_tokens;
    resp->output_tokens = st->output_tokens;

    for (int i = 0; i < st->tc_count && resp->call_count < MIMI_MAX_TOOL_CALLS; i++) {
        if (st->tc[i].name[0] == '\0') continue;
        llm_tool_call_t *call = &resp->calls[resp->call_count];
        strncpy(call->id,   st->tc[i].id,   sizeof(call->id)   - 1);
        strncpy(call->name, st->tc[i].name, sizeof(call->name) - 1);
        if (st->tc[i].args.data && st->tc[i].args.len > 0) {
            call->input     = strdup(st->tc[i].args.data);
            call->input_len = st->tc[i].args.len;
        } else {
            call->input     = strdup("{}");
            call->input_len = 2;
        }
        resp->call_count++;
    }

    return ESP_OK;
}

/* ── Public: chat with tools (streaming SSE) ─────────────────────── */

esp_err_t llm_chat_tools_streaming(const char *system_prompt,
                                   cJSON *messages,
                                   const char *tools_json,
                                   bool force_tool_use,
                                   llm_stream_progress_fn progress_cb,
                                   void *progress_ctx,
                                   llm_response_t *resp)
{
    /* Proxy path doesn't support SSE — delegate to blocking path */
    if (http_proxy_is_enabled()) {
        return llm_chat_tools(system_prompt, messages, tools_json, force_tool_use, resp);
    }

    memset(resp, 0, sizeof(*resp));
    if (s_api_key[0] == '\0' && !provider_is_ollama()) return ESP_ERR_INVALID_STATE;

    {
        char llm_info[96];
        snprintf(llm_info, sizeof(llm_info), "provider=%s model=%s", effective_provider(), effective_model());
        ws_server_broadcast_monitor("llm", llm_info);
    }

    /* Build request body (identical to non-streaming + stream:true) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", effective_model());
    if (provider_is_openai()) {
        cJSON_AddNumberToObject(body, "max_completion_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }
    cJSON_AddBoolToObject(body, "stream", true);
    if (provider_is_ollama()) {
        /* Keep model loaded for 10 min after last request (Ollama default is 5 min) */
        cJSON_AddStringToObject(body, "keep_alive", "10m");
    }

    if (provider_uses_openai_format()) {
        /* Request usage in the final streaming chunk */
        cJSON *sopts = cJSON_CreateObject();
        cJSON_AddBoolToObject(sopts, "include_usage", true);
        cJSON_AddItemToObject(body, "stream_options", sopts);

        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);
        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                cJSON_AddStringToObject(body, "tool_choice",
                                        force_tool_use ? "required" : "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt);
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                if (force_tool_use) {
                    cJSON *tc = cJSON_CreateObject();
                    cJSON_AddStringToObject(tc, "type", "any");
                    cJSON_AddItemToObject(body, "tool_choice", tc);
                }
            }
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling LLM streaming (provider: %s, model: %s, body: %d bytes)",
             effective_provider(), effective_model(), (int)strlen(post_data));

    /* Allocate SSE state */
    sse_state_t *st = sse_state_alloc();
    if (!st) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }
    st->progress_cb  = progress_cb;
    st->progress_ctx = progress_ctx;

    /* Hard stream deadline: 3 min for local plaintext, 90s for cloud TLS.
     * Prevents slow vision/reasoning models from blocking the agent task
     * indefinitely — the per-chunk timeout_ms does not bound total duration. */
    {
        int hard_ms = provider_is_plaintext() ? (3 * 60 * 1000) : (90 * 1000);
        st->stream_deadline_us = esp_timer_get_time() + (int64_t)hard_ms * 1000;
    }

    /* Configure HTTP client with SSE event handler */
    esp_http_client_config_t config = {
        .url              = llm_api_url(),
        .event_handler    = sse_http_event_handler,
        .user_data        = st,
        .timeout_ms       = provider_is_plaintext() ? 240 * 1000 : 120 * 1000,
        .buffer_size      = 4096,
        .buffer_size_tx   = 4096,
        .crt_bundle_attach = provider_is_plaintext() ? NULL : esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        sse_state_free(st);
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    if (provider_uses_openai_format()) {
        if (s_api_key[0]) {
            char auth[LLM_API_KEY_MAX_LEN + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
            esp_http_client_set_header(client, "Authorization", auth);
        }
        if (provider_is_openrouter()) {
            esp_http_client_set_header(client, "HTTP-Referer", MIMI_OPENROUTER_REFERER);
            esp_http_client_set_header(client, "X-Title",      MIMI_OPENROUTER_TITLE);
        }
    } else {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", MIMI_LLM_API_VERSION);
    }
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK) {
        char emsg[80];
        snprintf(emsg, sizeof(emsg), "LLM streaming HTTP failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", emsg);
        ws_server_broadcast_monitor("error", emsg);
        sse_state_free(st);
        return err;
    }

    if (status != 200) {
        char emsg[256];
        /* Include error body from SSE buffer if available */
        const char *body = (st->text.len > 0) ? st->text.data : "(no body)";
        snprintf(emsg, sizeof(emsg), "LLM API error %d (streaming): %.180s", status, body);
        for (char *p = emsg; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        ESP_LOGE(TAG, "%s", emsg);
        ws_server_broadcast_monitor("error", emsg);
        sse_state_free(st);
        return ESP_FAIL;
    }

    {
        char szlog[64];
        snprintf(szlog, sizeof(szlog), "LLM stream done: %u bytes text, %d tool calls",
                 (unsigned)st->text.len, st->tc_count);
        ws_server_broadcast_monitor("llm", szlog);
    }

    /* Convert accumulated SSE state to structured response */
    err = sse_state_to_response(st, resp);
    sse_state_free(st);

    if (err != ESP_OK) return err;

    /* Update session stats */
    s_total_input_tokens  += resp->input_tokens;
    s_total_output_tokens += resp->output_tokens;

    {
        char summary[96];
        snprintf(summary, sizeof(summary), "LLM: %lu\u2191 %lu\u2193 tok, %d tool%s, stop=%s",
                 (unsigned long)resp->input_tokens, (unsigned long)resp->output_tokens,
                 resp->call_count, resp->call_count == 1 ? "" : "s",
                 resp->tool_use ? "tool_use" : "end_turn");
        ws_server_broadcast_monitor("llm", summary);
    }

    ESP_LOGI(TAG, "Stream response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_use" : "end_turn");

    return ESP_OK;
}

/* ── Session stats ────────────────────────────────────────────── */

void llm_get_session_stats(uint32_t *in, uint32_t *out, uint32_t *cost_millicents)
{
    if (in)             *in             = s_total_input_tokens;
    if (out)            *out            = s_total_output_tokens;
    if (cost_millicents) *cost_millicents = s_total_cost_millicents;
}

const char *llm_get_provider(void)  { return effective_provider(); }
const char *llm_get_model(void)    { return effective_model(); }
const char *llm_get_api_key(void)  { return s_api_key; }
const char *llm_get_local_url(void) { return s_local_url; }

void llm_set_local_url(const char *url)
{
    if (url) {
        safe_copy(s_local_url, sizeof(s_local_url), url);
        ESP_LOGI(TAG, "Local model URL set to: %s", s_local_url);
    } else {
        s_local_url[0] = '\0';
    }
}

/* ── NVS helpers ──────────────────────────────────────────────── */

esp_err_t llm_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err)); return err; }
    err = nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs set_api_key failed: %s", esp_err_to_name(err)); return err; }

    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    ESP_LOGI(TAG, "API key saved");
    return ESP_OK;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err)); return err; }
    err = nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs set_model failed: %s", esp_err_to_name(err)); return err; }

    safe_copy(s_model, sizeof(s_model), model);
    ESP_LOGI(TAG, "Model set to: %s", s_model);
    return ESP_OK;
}

esp_err_t llm_set_provider(const char *provider)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err)); return err; }
    err = nvs_set_str(nvs, MIMI_NVS_KEY_PROVIDER, provider);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs set_provider failed: %s", esp_err_to_name(err)); return err; }

    safe_copy(s_provider, sizeof(s_provider), provider);
    ESP_LOGI(TAG, "Provider set to: %s", s_provider);
    return ESP_OK;
}

/* ── Per-request override (for smart routing) ─────────────────── */

void llm_set_request_override(const char *provider, const char *model)
{
    safe_copy(s_override_provider, sizeof(s_override_provider), provider);
    safe_copy(s_override_model, sizeof(s_override_model), model);
    s_override_active = true;
    ESP_LOGI(TAG, "Request override: provider=%s model=%s", s_override_provider, s_override_model);
}

void llm_clear_request_override(void)
{
    s_override_active = false;
    s_override_provider[0] = '\0';
    s_override_model[0] = '\0';
}

void llm_set_local_model(const char *model)
{
    if (model) {
        safe_copy(s_local_model, sizeof(s_local_model), model);
        ESP_LOGI(TAG, "Local model set to: %s", s_local_model);
    } else {
        s_local_model[0] = '\0';
    }
}

const char *llm_get_local_model(void) { return s_local_model; }

void llm_set_local_text_model(const char *model)
{
    if (model && model[0]) {
        safe_copy(s_local_text_model, sizeof(s_local_text_model), model);
        ESP_LOGI(TAG, "Local text model set to: %s", s_local_text_model);
    } else {
        s_local_text_model[0] = '\0';
    }
}

/* Returns the text-only local model, falling back to s_local_model if not set. */
const char *llm_get_local_text_model(void)
{
    return (s_local_text_model[0]) ? s_local_text_model : s_local_model;
}

bool llm_local_health_check(void)
{
    if (!s_local_url[0]) return false;

    /* Use cached result if fresh */
    int64_t now = esp_timer_get_time();
    if (s_local_check_us > 0 && (now - s_local_check_us) < LOCAL_HEALTH_CACHE_US) {
        return s_local_online;
    }

    /* HTTP GET to /v1/models with short timeout */
    char url[LLM_LOCAL_URL_MAX + 16];
    snprintf(url, sizeof(url), "%s/models", s_local_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        s_local_online = false;
        s_local_check_us = now;
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    s_local_online = (err == ESP_OK && status == 200);
    s_local_check_us = now;
    ESP_LOGI(TAG, "Local health check: %s (status=%d)", s_local_online ? "online" : "offline", status);
    return s_local_online;
}

bool llm_smart_routing_available(void)
{
    /* Smart routing is possible when: global provider is cloud-based AND local URL+model are configured */
    return (strcmp(s_provider, "openrouter") == 0 || strcmp(s_provider, "anthropic") == 0)
           && s_local_url[0] && s_local_model[0];
}

/* ── Voice routing ──────────────────────────────────────────────── */

void llm_set_voice_provider(const char *p)
{
    if (p) safe_copy(s_voice_provider, sizeof(s_voice_provider), p);
    else    s_voice_provider[0] = '\0';
}

void llm_set_voice_model(const char *m)
{
    if (m) safe_copy(s_voice_model, sizeof(s_voice_model), m);
    else    s_voice_model[0] = '\0';
}

bool llm_voice_routing_available(void)
{
    return s_voice_provider[0] != '\0' && s_voice_model[0] != '\0';
}

const char *llm_get_voice_provider(void) { return s_voice_provider; }
const char *llm_get_voice_model(void)    { return s_voice_model; }

/* ── Ollama boot warmup ─────────────────────────────────────────── */

static void llm_warmup_task(void *arg)
{
    /* Wait for WiFi + services to settle, and for Ollama to load the model.
     * qwen2.5:14b (~9GB) takes longer to load than llama3.2:3b (~2GB),
     * so use 25s to avoid a premature "warmup done" before the model is ready. */
    vTaskDelay(pdMS_TO_TICKS(25000));

    if (!s_local_url[0] || !s_local_model[0]) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Ollama warmup: loading %s...", s_local_model);

    /* Build minimal chat completion — no tools, no system prompt, stream=false */
    char url[LLM_LOCAL_URL_MAX + 32];
    snprintf(url, sizeof(url), "%s/chat/completions", s_local_url);

    char body[384];
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\","
             "\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],"
             "\"max_tokens\":3,\"stream\":false}",
             s_local_model);

    /* No response buffer needed — we only care about HTTP status code */
    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = 60000,  /* model cold-load can take up to 60s */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { vTaskDelete(NULL); return; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = esp_http_client_perform(client);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Ollama warmup done in %lldms — model ready", elapsed_ms);
        /* Prime health cache so first agent turn skips the health check */
        s_local_online   = true;
        s_local_check_us = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "Ollama warmup failed in %lldms (err=%s HTTP %d) — will retry on first request",
                 elapsed_ms, esp_err_to_name(ret), status);
    }

    vTaskDelete(NULL);
}
