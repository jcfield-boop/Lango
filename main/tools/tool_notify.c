#include "tool_notify.h"
#include "gateway/ws_server.h"
#include "langoustine_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "tool_notify";

#define NOTIFY_NVS_NS         "notify_config"
#define NOTIFY_NVS_KEY_TOPIC  "topic"
#define NOTIFY_NVS_KEY_SERVER "server"
#define NOTIFY_DEFAULT_SERVER "https://ntfy.sh"
#define NOTIFY_RESP_BUF_SIZE  512

typedef struct {
    char data[NOTIFY_RESP_BUF_SIZE];
    int  len;
} notify_resp_t;

static esp_err_t notify_event_cb(esp_http_client_event_t *evt)
{
    notify_resp_t *r = (notify_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (r->len + copy > (int)sizeof(r->data) - 1)
            copy = (int)sizeof(r->data) - 1 - r->len;
        if (copy > 0) {
            memcpy(r->data + r->len, evt->data, copy);
            r->len += copy;
            r->data[r->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t tool_notify_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_msg      = cJSON_GetObjectItem(input, "message");
    cJSON *j_title    = cJSON_GetObjectItem(input, "title");
    cJSON *j_priority = cJSON_GetObjectItem(input, "priority");
    cJSON *j_tags     = cJSON_GetObjectItem(input, "tags");
    cJSON *j_topic    = cJSON_GetObjectItem(input, "topic");

    if (!j_msg || !cJSON_IsString(j_msg) || !j_msg->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'message' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *message  = j_msg->valuestring;
    const char *title    = (j_title    && cJSON_IsString(j_title))    ? j_title->valuestring    : NULL;
    const char *priority = (j_priority && cJSON_IsString(j_priority)) ? j_priority->valuestring : NULL;
    const char *tags     = (j_tags     && cJSON_IsString(j_tags))     ? j_tags->valuestring     : NULL;

    /* Resolve topic: input JSON > NVS default */
    char topic[128] = {0};
    if (j_topic && cJSON_IsString(j_topic) && j_topic->valuestring[0]) {
        snprintf(topic, sizeof(topic), "%s", j_topic->valuestring);
    } else {
        nvs_handle_t nvs;
        if (nvs_open(NOTIFY_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(topic);
            nvs_get_str(nvs, NOTIFY_NVS_KEY_TOPIC, topic, &len);
            nvs_close(nvs);
        }
    }

    if (!topic[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: no ntfy topic set. Use 'set_notify_topic <topic>' or pass 'topic' in input.");
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolve server */
    char server[128] = NOTIFY_DEFAULT_SERVER;
    {
        nvs_handle_t nvs;
        if (nvs_open(NOTIFY_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(server);
            nvs_get_str(nvs, NOTIFY_NVS_KEY_SERVER, server, &len);
            nvs_close(nvs);
        }
    }
    /* Strip trailing slash */
    int slen = strlen(server);
    if (slen > 0 && server[slen - 1] == '/') server[slen - 1] = '\0';

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", server, topic);

    notify_resp_t resp = {0};

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = notify_event_cb,
        .user_data         = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    /* Set optional headers */
    if (title)    esp_http_client_set_header(client, "Title",    title);
    if (priority) esp_http_client_set_header(client, "Priority", priority);
    if (tags)     esp_http_client_set_header(client, "Tags",     tags);
    esp_http_client_set_header(client, "Content-Type", "text/plain");

    /* Set POST body */
    esp_http_client_set_post_field(client, message, (int)strlen(message));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(input);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: HTTP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ntfy POST %s → HTTP %d", url, status);

    char vlog[192];
    snprintf(vlog, sizeof(vlog), "send_notification -> %.100s (HTTP %d)", topic, status);
    ws_server_broadcast_monitor_verbose("tool", vlog);

    if (status >= 200 && status < 300) {
        snprintf(output, output_size, "Notification sent to topic '%s'.", topic);
        return ESP_OK;
    } else {
        snprintf(output, output_size, "Error: ntfy returned HTTP %d: %.200s", status, resp.data);
        return ESP_FAIL;
    }
}
