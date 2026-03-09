#include "tool_http.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_http";

#define HTTP_RESP_BUF_SIZE (8 * 1024)

typedef struct {
    char *data;
    int   len;
    int   cap;
} http_resp_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (r->len + copy > r->cap - 1) copy = r->cap - 1 - r->len;
        if (copy > 0) {
            memcpy(r->data + r->len, evt->data, copy);
            r->len += copy;
            r->data[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool url_is_allowed(const char *url, char *err_out, size_t err_size)
{
    if (!url || !url[0]) {
        snprintf(err_out, err_size, "Error: empty URL");
        return false;
    }
    bool is_http  = strncasecmp(url, "http://",  7) == 0;
    bool is_https = strncasecmp(url, "https://", 8) == 0;
    if (!is_http && !is_https) {
        snprintf(err_out, err_size, "Error: only http/https URLs are allowed");
        return false;
    }
    const char *host_start = is_https ? url + 8 : url + 7;
    char host[128] = {0};
    size_t i = 0;
    while (host_start[i] && host_start[i] != '/' && host_start[i] != ':' && i < sizeof(host) - 1) {
        host[i] = host_start[i];
        i++;
    }
    host[i] = '\0';
    /* Block loopback and RFC-1918 private ranges */
    if (strcasecmp(host, "localhost") == 0 ||
        strncmp(host, "127.", 4)      == 0 ||
        strncmp(host, "169.254.", 8)  == 0 ||
        strncmp(host, "10.", 3)       == 0 ||
        strncmp(host, "192.168.", 8)  == 0) {
        snprintf(err_out, err_size, "Error: access to internal/private addresses is blocked");
        return false;
    }
    /* Block 172.16.0.0/12 */
    if (strncmp(host, "172.", 4) == 0) {
        int second_octet = 0;
        if (sscanf(host + 4, "%d", &second_octet) == 1 &&
            second_octet >= 16 && second_octet <= 31) {
            snprintf(err_out, err_size, "Error: access to internal/private addresses is blocked");
            return false;
        }
    }
    return true;
}

esp_err_t tool_http_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_url     = cJSON_GetObjectItem(input, "url");
    cJSON *j_method  = cJSON_GetObjectItem(input, "method");
    cJSON *j_headers = cJSON_GetObjectItem(input, "headers");
    cJSON *j_body    = cJSON_GetObjectItem(input, "body");

    if (!j_url || !cJSON_IsString(j_url)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'url' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *url    = j_url->valuestring;

    if (!url_is_allowed(url, output, output_size)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    const char *method = (j_method && cJSON_IsString(j_method)) ? j_method->valuestring : "GET";
    const char *body   = (j_body   && cJSON_IsString(j_body))   ? j_body->valuestring   : NULL;
    bool is_post = (strcasecmp(method, "POST") == 0);

    http_resp_t resp = {0};
    resp.data = ps_malloc(HTTP_RESP_BUF_SIZE);
    if (!resp.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';
    resp.cap = HTTP_RESP_BUF_SIZE;

    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms       = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler    = http_event_cb,
        .user_data        = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp.data);
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    /* Apply custom headers — validate name (alphanum + '-') and strip CR/LF from value */
    if (j_headers && cJSON_IsObject(j_headers)) {
        cJSON *h;
        cJSON_ArrayForEach(h, j_headers) {
            if (!cJSON_IsString(h)) continue;
            /* Validate header name: only alphanumeric and '-' */
            bool name_ok = true;
            for (const char *p = h->string; *p; p++) {
                if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= '0' && *p <= '9') || *p == '-')) {
                    name_ok = false;
                    break;
                }
            }
            if (!name_ok) continue;
            /* Strip CR and LF from header value */
            char safe_val[256];
            size_t vi = 0;
            for (const char *p = h->valuestring; *p && vi < sizeof(safe_val) - 1; p++) {
                if (*p != '\r' && *p != '\n') safe_val[vi++] = *p;
            }
            safe_val[vi] = '\0';
            esp_http_client_set_header(client, h->string, safe_val);
        }
    }

    /* Set POST body */
    if (is_post && body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(input);

    if (err != ESP_OK) {
        free(resp.data);
        snprintf(output, output_size, "Error: HTTP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%s %s → HTTP %d (%d bytes)", method, url, status, resp.len);

    char vlog[128];
    snprintf(vlog, sizeof(vlog), "http_request %s → %d", url, status);
    ws_server_broadcast_monitor_verbose("tool", vlog);

    snprintf(output, output_size, "HTTP %d\n%.*s",
             status, (int)(output_size - 10), resp.data);
    free(resp.data);

    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}
