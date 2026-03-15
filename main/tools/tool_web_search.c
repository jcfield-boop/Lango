#include "tool_web_search.h"
#include "mimi_config.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};

static uint32_t s_total_searches        = 0;
static uint32_t s_search_cost_millicents = 0;

/* Per-turn rate limiting: cap searches per agent turn to limit runaway cost */
#define SEARCH_MAX_PER_TURN  5
static int s_search_calls_this_turn = 0;

void web_search_reset_turn(void)
{
    s_search_calls_this_turn = 0;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (Tavily, key configured)");
    } else {
        ESP_LOGW(TAG, "No Tavily API key. Add search_key to SERVICES.md or use `set_search_key <key>`");
    }
    return ESP_OK;
}

const char *tool_web_search_get_key(void) { return s_search_key; }

void tool_web_search_get_stats(uint32_t *calls, uint32_t *cost_millicents)
{
    if (calls)           *calls           = s_total_searches;
    if (cost_millicents) *cost_millicents = s_search_cost_millicents;
}

/* ── Tavily search ────────────────────────────────────────────── */

static esp_err_t search_tavily(const char *query, const char *api_key, char **out)
{
    char *buf = ps_malloc(MIMI_TAVILY_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    /* Tavily API v2: key in Authorization header (works for tvly- and re- keys).
     * Do NOT include api_key in the JSON body — Research API (re-) keys require
     * Bearer auth and return 422 if the key is embedded in the body. */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "query", query);
    cJSON_AddNumberToObject(body, "max_results", 5);
    cJSON_AddBoolToObject(body, "include_answer", true);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) { free(buf); return ESP_ERR_NO_MEM; }

    /* Build "Bearer <key>" auth header */
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    esp_http_client_config_t cfg = {
        .url = "https://api.tavily.com/search",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    int body_len = (int)strlen(body_str);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_open(client, body_len);
    int total = 0;
    if (err == ESP_OK) {
        int written = esp_http_client_write(client, body_str, body_len);
        if (written < 0) {
            ws_server_broadcast_monitor("error", "Tavily: write failed");
            err = ESP_FAIL;
        } else {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status != 200) {
                /* Read and log error body for diagnostics */
                char errbuf[256] = {0};
                int rd = esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
                if (rd > 0) errbuf[rd] = '\0';
                ESP_LOGW(TAG, "Tavily HTTP %d: %.200s", status, errbuf);
                char emsg[96];
                snprintf(emsg, sizeof(emsg), "Tavily HTTP %d: %.60s", status, errbuf);
                ws_server_broadcast_monitor("error", emsg);
                err = ESP_FAIL;
            } else {
                int rd;
                while ((rd = esp_http_client_read(client, buf + total,
                                                   MIMI_TAVILY_BUF_SIZE - total - 1)) > 0) {
                    total += rd;
                }
                buf[total] = '\0';
            }
        }
    }
    esp_http_client_cleanup(client);
    free(body_str);

    if (err != ESP_OK || total == 0) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_ParseWithLength(buf, total);
    free(buf);
    if (!root) return ESP_FAIL;

    cJSON *answer  = cJSON_GetObjectItem(root, "answer");
    cJSON *results = cJSON_GetObjectItem(root, "results");

    char *output = ps_malloc(4096);
    if (!output) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    int pos = 0;

    if (answer && cJSON_IsString(answer) && strlen(answer->valuestring) > 10) {
        pos += snprintf(output + pos, 4096 - pos, "%s\n\n", answer->valuestring);
    }
    if (results && cJSON_IsArray(results)) {
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n && i < 5 && pos < 3800; i++) {
            cJSON *item  = cJSON_GetArrayItem(results, i);
            cJSON *title = cJSON_GetObjectItem(item, "title");
            cJSON *url   = cJSON_GetObjectItem(item, "url");
            cJSON *snip  = cJSON_GetObjectItem(item, "content");
            if (title && url) {
                pos += snprintf(output + pos, 4096 - pos, "[%d] %s\n%s\n",
                                i + 1,
                                cJSON_IsString(title) ? title->valuestring : "",
                                cJSON_IsString(url)   ? url->valuestring   : "");
            }
            if (snip && cJSON_IsString(snip) && pos < 3600) {
                pos += snprintf(output + pos, 4096 - pos, "%.200s\n\n", snip->valuestring);
            }
        }
    }
    cJSON_Delete(root);

    if (pos == 0) { free(output); return ESP_FAIL; }
    *out = output;
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size,
                 "Search not available: Tavily API key not configured. "
                 "Add search_key to SERVICES.md or use `set_search_key <key>` in CLI.");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_search_calls_this_turn >= SEARCH_MAX_PER_TURN) {
        snprintf(output, output_size,
                 "Search limit reached: max %d searches per turn to control costs. "
                 "Use the information already gathered to answer the question.",
                 SEARCH_MAX_PER_TURN);
        return ESP_ERR_INVALID_STATE;
    }
    s_search_calls_this_turn++;

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);
    ws_server_broadcast_monitor_verbose("search", "Tavily");

    char *result = NULL;
    esp_err_t err = search_tavily(query->valuestring, s_search_key, &result);
    cJSON_Delete(input);

    if (err == ESP_OK && result) {
        snprintf(output, output_size, "%s", result);
        free(result);
        s_total_searches++;
        s_search_cost_millicents += 200; /* Tavily: ~$0.002/query = 200 millicents */
    } else {
        free(result);
        snprintf(output, output_size, "Error: Tavily search failed");
    }
    return err;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}
