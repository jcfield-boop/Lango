#include "tool_capture_image.h"
#include "camera/uvc_camera.h"
#include "langoustine_config.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "llm/llm_proxy.h"
#include "mbedtls/base64.h"

static const char *TAG = "tool_img";

#define API_KEY_MAX   320
#define MODEL_MAX      64
#define PROVIDER_MAX   32
#define PROMPT_MAX    256

/* ── Response accumulator ──────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_append(resp_buf_t *rb, const char *src, size_t n)
{
    if (rb->len + n >= rb->cap) {
        size_t new_cap = rb->cap + n + 1024;
        char *tmp = ps_realloc(rb->data, new_cap);
        if (!tmp) return ESP_ERR_NO_MEM;
        rb->data = tmp;
        rb->cap  = new_cap;
    }
    memcpy(rb->data + rb->len, src, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static esp_err_t vision_http_event_cb(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (resp_append(rb, (const char *)evt->data, evt->data_len) != ESP_OK) {
            ESP_LOGE(TAG, "resp_append OOM: %d bytes dropped (buf len=%u)", evt->data_len, (unsigned)rb->len);
        }
    }
    return ESP_OK;
}

/* ── Save JPEG to LittleFS ─────────────────────────────────────── */

static esp_err_t save_jpeg(const uint8_t *data, size_t len)
{
    /* Ensure capture directory exists */
    struct stat st;
    if (stat(LANG_CAMERA_CAPTURE_DIR, &st) != 0) {
        mkdir(LANG_CAMERA_CAPTURE_DIR, 0755);
    }

    FILE *f = fopen(LANG_CAMERA_CAPTURE_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", LANG_CAMERA_CAPTURE_PATH);
        return ESP_FAIL;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "Saved %u bytes → %s", (unsigned)len, LANG_CAMERA_CAPTURE_PATH);
    return ESP_OK;
}

/* ── Call Claude vision API ────────────────────────────────────── */

/*
 * Builds the JSON request body in a single PSRAM allocation:
 *   {"model":"<m>","max_tokens":<t>,"messages":[{"role":"user","content":[
 *     {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":"<b64>"}},
 *     {"type":"text","text":"<prompt>"}
 *   ]}]}
 *
 * Returns heap-allocated body string (ps_malloc) or NULL on OOM.
 */
static char *build_vision_body(const uint8_t *jpeg, size_t jpeg_len,
                                const char *model, int max_tokens,
                                const char *prompt, size_t *body_len_out)
{
    /* Calculate base64 output size */
    size_t b64_max = ((jpeg_len + 2) / 3) * 4 + 1;

    /* Allocate: b64 data + JSON envelope (~512 bytes overhead) */
    size_t body_cap = b64_max + 512 + PROMPT_MAX + MODEL_MAX;
    char *body = ps_malloc(body_cap);
    if (!body) return NULL;

    /* JSON prefix */
    int prefix_len = snprintf(body, body_cap,
        "{\"model\":\"%s\","
        "\"max_tokens\":%d,"
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image\",\"source\":"
        "{\"type\":\"base64\",\"media_type\":\"image/jpeg\","
        "\"data\":\"",
        model, max_tokens);
    if (prefix_len < 0 || (size_t)prefix_len >= body_cap) {
        free(body);
        return NULL;
    }

    /* Base64-encode JPEG directly into body buffer after prefix */
    size_t b64_written = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)body + prefix_len,
        body_cap - (size_t)prefix_len,
        &b64_written,
        jpeg, jpeg_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 encode failed (%d)", rc);
        free(body);
        return NULL;
    }

    /* JSON suffix */
    int suffix_len = snprintf(body + prefix_len + b64_written,
        body_cap - (size_t)prefix_len - b64_written,
        "\"}},"
        "{\"type\":\"text\",\"text\":\"%s\"}"
        "]}]}",
        prompt);
    if (suffix_len < 0) {
        free(body);
        return NULL;
    }

    *body_len_out = (size_t)prefix_len + b64_written + (size_t)suffix_len;
    return body;
}

/* ── OpenAI/OpenRouter vision body builder ─────────────────────── */

static char *build_vision_body_openai(const uint8_t *jpeg, size_t jpeg_len,
                                       const char *model, int max_tokens,
                                       const char *prompt, size_t *body_len_out)
{
    /* "data:image/jpeg;base64," prefix embedded in the URL field */
    static const char DATA_PREFIX[] = "data:image/jpeg;base64,";
    size_t b64_max    = ((jpeg_len + 2) / 3) * 4 + 1;
    size_t data_pfx   = sizeof(DATA_PREFIX) - 1;
    size_t body_cap   = b64_max + data_pfx + 512 + PROMPT_MAX + MODEL_MAX;
    char  *body       = ps_malloc(body_cap);
    if (!body) return NULL;

    int prefix_len = snprintf(body, body_cap,
        "{\"model\":\"%s\","
        "\"max_tokens\":%d,"
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,",
        model, max_tokens);
    if (prefix_len < 0 || (size_t)prefix_len >= body_cap) { free(body); return NULL; }

    size_t b64_written = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)body + prefix_len,
        body_cap - (size_t)prefix_len,
        &b64_written, jpeg, jpeg_len);
    if (rc != 0) { ESP_LOGE(TAG, "base64 encode failed (%d)", rc); free(body); return NULL; }

    int suffix_len = snprintf(body + prefix_len + b64_written,
        body_cap - (size_t)prefix_len - b64_written,
        "\"}},"
        "{\"type\":\"text\",\"text\":\"%s\"}"
        "]}]}",
        prompt);
    if (suffix_len < 0) { free(body); return NULL; }

    *body_len_out = (size_t)prefix_len + b64_written + (size_t)suffix_len;
    return body;
}

/* ── Main tool entry point ─────────────────────────────────────── */

esp_err_t tool_capture_image_execute(const char *input_json,
                                     char *output, size_t output_size)
{
    /* 1. Parse optional prompt from input JSON */
    char prompt[PROMPT_MAX] = "Describe what you see in this image in detail.";
    if (input_json && input_json[0]) {
        cJSON *j = cJSON_Parse(input_json);
        if (j) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "prompt");
            if (cJSON_IsString(p) && p->valuestring && p->valuestring[0]) {
                strncpy(prompt, p->valuestring, sizeof(prompt) - 1);
            }
            cJSON_Delete(j);
        }
    }

    /* 2. Check camera */
    if (!uvc_camera_is_connected()) {
        snprintf(output, output_size,
                 "Error: no USB webcam detected. Connect a UVC-compatible "
                 "webcam to the USB-A port.");
        return ESP_ERR_NOT_FOUND;
    }

    /* 3. Allocate PSRAM frame buffer */
    uint8_t *jpeg_buf = ps_malloc(LANG_CAMERA_BUF_SIZE);
    if (!jpeg_buf) {
        snprintf(output, output_size, "Error: out of PSRAM for frame buffer");
        return ESP_ERR_NO_MEM;
    }

    /* 4. Capture frame */
    size_t jpeg_len = 0;
    esp_err_t err = uvc_camera_capture(jpeg_buf, LANG_CAMERA_BUF_SIZE,
                                       &jpeg_len, LANG_CAMERA_CAPTURE_TIMEOUT_MS);
    if (err != ESP_OK) {
        free(jpeg_buf);
        snprintf(output, output_size,
                 "Error: capture failed (%s)", esp_err_to_name(err));
        return err;
    }

    /* 5. Save to LittleFS */
    save_jpeg(jpeg_buf, jpeg_len);  /* best-effort */

    /* 6. Get API key, model, provider from llm_proxy (already loaded from NVS at boot) */
    char api_key[API_KEY_MAX] = {0};
    char model[MODEL_MAX]     = {0};

    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(api_key);
        nvs_get_str(nvs, LANG_NVS_KEY_API_KEY, api_key, &len);
        nvs_close(nvs);
    }
    if (!api_key[0]) strncpy(api_key, LANG_SECRET_API_KEY, sizeof(api_key) - 1);

    const char *provider = llm_get_provider();
    strncpy(model, llm_get_model(), sizeof(model) - 1);

    if (!api_key[0]) {
        free(jpeg_buf);
        snprintf(output, output_size,
                 "Image captured (%u bytes) and saved to %s. "
                 "No API key configured — set one via CLI to enable vision.",
                 (unsigned)jpeg_len, LANG_CAMERA_CAPTURE_PATH);
        return ESP_OK;
    }

    /* Determine provider: "openrouter" or "openai" use OpenAI-compat format */
    bool use_openai = (strcmp(provider, "openrouter") == 0 ||
                       strcmp(provider, "openai")     == 0);
    const char *api_url = use_openai
        ? (strcmp(provider, "openrouter") == 0 ? LANG_OPENROUTER_API_URL : LANG_OPENAI_API_URL)
        : LANG_LLM_API_URL;
    ESP_LOGI(TAG, "Vision API: provider=%s model=%s url=%s", provider, model, api_url);

    /* 7. Build JSON body */
    size_t body_len = 0;
    char *body = use_openai
        ? build_vision_body_openai(jpeg_buf, jpeg_len, model, LANG_VISION_MAX_TOKENS, prompt, &body_len)
        : build_vision_body(jpeg_buf, jpeg_len, model, LANG_VISION_MAX_TOKENS, prompt, &body_len);
    free(jpeg_buf);

    if (!body) {
        snprintf(output, output_size, "Error: failed to build vision request body");
        return ESP_ERR_NO_MEM;
    }

    /* 8. Allocate response buffer */
    resp_buf_t rb = { .data = ps_malloc(8192), .len = 0, .cap = 8192 };
    if (!rb.data) {
        free(body);
        snprintf(output, output_size, "Error: out of memory for response buffer");
        return ESP_ERR_NO_MEM;
    }
    rb.data[0] = '\0';

    /* 9. POST to vision API */
    esp_http_client_config_t cfg = {
        .url               = api_url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 30000,
        .buffer_size       = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = vision_http_event_cb,
        .user_data         = &rb,
        .buffer_size_tx    = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        free(rb.data);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (use_openai) {
        char auth_bearer[API_KEY_MAX + 16];
        snprintf(auth_bearer, sizeof(auth_bearer), "Bearer %s", api_key);
        esp_http_client_set_header(client, "Authorization", auth_bearer);
        if (strncmp(provider, "openrouter", 10) == 0) {
            esp_http_client_set_header(client, "HTTP-Referer", LANG_OPENROUTER_REFERER);
            esp_http_client_set_header(client, "X-Title",      LANG_OPENROUTER_TITLE);
        }
    } else {
        esp_http_client_set_header(client, "x-api-key", api_key);
        esp_http_client_set_header(client, "anthropic-version", LANG_LLM_API_VERSION);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Vision API HTTP %d: %.400s", status,
                 rb.data[0] ? rb.data : "(no body)");
        /* Save error body to LittleFS so it can be fetched for debugging */
        if (rb.data[0]) {
            FILE *ef = fopen("/lfs/captures/vision_err.json", "w");
            if (ef) { fwrite(rb.data, 1, rb.len, ef); fclose(ef); }
        }
        snprintf(output, output_size,
                 "Error: vision API returned HTTP %d", status);
        free(rb.data);
        return ESP_FAIL;
    }

    /* 10. Parse response — Anthropic: content[0].text  OpenAI: choices[0].message.content */
    cJSON *resp = cJSON_Parse(rb.data);
    free(rb.data);

    if (!resp) {
        snprintf(output, output_size, "Error: failed to parse vision API response");
        return ESP_FAIL;
    }

    const char *description = NULL;
    if (use_openai) {
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *msg  = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "message");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (cJSON_IsString(text)) description = text->valuestring;
        }
    } else {
        cJSON *content_arr = cJSON_GetObjectItemCaseSensitive(resp, "content");
        if (cJSON_IsArray(content_arr) && cJSON_GetArraySize(content_arr) > 0) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content_arr, 0), "text");
            if (cJSON_IsString(text)) description = text->valuestring;
        }
    }

    if (description) {
        snprintf(output, output_size, "%s", description);
    } else {
        snprintf(output, output_size,
                 "Image captured (%u bytes, saved to %s). "
                 "Vision API returned no description.",
                 (unsigned)jpeg_len, LANG_CAMERA_CAPTURE_PATH);
    }

    cJSON_Delete(resp);
    return ESP_OK;
}
