#include "tool_frame_tv.h"
#include "langoustine_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "tool_frame_tv";

/* Default: Mac running Nanoframe on the local audio server host */
#ifndef LANG_NANOFRAME_URL
#define LANG_NANOFRAME_URL "http://192.168.0.51:11436/generate"
#endif

#define RESP_BUF_SIZE 256
#define HTTP_TIMEOUT_MS 15000   /* just waiting for 202 acceptance */

typedef struct {
    char buf[RESP_BUF_SIZE];
    int  len;
} resp_t;

static esp_err_t http_cb(esp_http_client_event_t *evt)
{
    resp_t *r = (resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r->len < RESP_BUF_SIZE - 1) {
        int n = evt->data_len;
        if (r->len + n > RESP_BUF_SIZE - 1) n = RESP_BUF_SIZE - 1 - r->len;
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
    }
    return ESP_OK;
}

esp_err_t tool_frame_tv_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Invalid tool input");
        return ESP_FAIL;
    }

    cJSON *prompt_item = cJSON_GetObjectItem(input, "prompt");
    if (!cJSON_IsString(prompt_item) || !prompt_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Missing prompt");
        return ESP_FAIL;
    }

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "prompt", prompt_item->valuestring);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(input);

    if (!body_str) {
        snprintf(output, output_size, "Out of memory");
        return ESP_FAIL;
    }

    resp_t resp = {0};

    esp_http_client_config_t cfg = {
        .url         = LANG_NANOFRAME_URL,
        .method      = HTTP_METHOD_POST,
        .timeout_ms  = HTTP_TIMEOUT_MS,
        .event_handler = http_cb,
        .user_data   = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP error: %s", esp_err_to_name(err));
        snprintf(output, output_size,
                 "Couldn't reach the Nanoframe app — make sure it's running on the Mac. "
                 "(err: %s)", esp_err_to_name(err));
        return ESP_OK;
    }

    if (status != 200 && status != 202) {
        ESP_LOGW(TAG, "Nanoframe returned HTTP %d", status);
        snprintf(output, output_size,
                 "Nanoframe app returned an error (HTTP %d). "
                 "Make sure it is running and the OpenAI key is configured.", status);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Frame TV generation triggered (HTTP %d)", status);
    snprintf(output, output_size,
             "Generating the image now — it will appear on the Frame TV in about 30–60 seconds.");
    return ESP_OK;
}
