#include "tool_web_search.h"
#include "mimi_config.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};

static uint32_t s_total_searches        = 0;
static uint32_t s_search_cost_millicents = 0;

/* Per-turn rate limiting: cap searches per agent turn to limit runaway cost */
#define SEARCH_MAX_PER_TURN  5
static int s_search_calls_this_turn = 0;

/* ── Result cache (PSRAM, avoids re-querying identical searches) ──── */
#define CACHE_SLOTS     8
#define CACHE_TTL_US    (5 * 60 * 1000000LL)   /* 5 minutes */
#define CACHE_MAX_BYTES 4096                     /* max cached result size */

typedef struct {
    uint32_t hash;
    int64_t  ts_us;
    char    *text;          /* ps_malloc'd copy, NULL = empty slot */
} search_cache_entry_t;

static search_cache_entry_t s_cache[CACHE_SLOTS];

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static const char *cache_lookup(uint32_t hash)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].text && s_cache[i].hash == hash &&
            (now - s_cache[i].ts_us) < CACHE_TTL_US) {
            return s_cache[i].text;
        }
    }
    return NULL;
}

static void cache_store(uint32_t hash, const char *text)
{
    /* Find oldest or empty slot */
    int best = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!s_cache[i].text) { best = i; break; }
        if (s_cache[i].ts_us < oldest) { oldest = s_cache[i].ts_us; best = i; }
    }
    free(s_cache[best].text);  /* ps_malloc'd or NULL */
    size_t len = strlen(text);
    if (len >= CACHE_MAX_BYTES) len = CACHE_MAX_BYTES - 1;
    s_cache[best].text = ps_malloc(len + 1);
    if (s_cache[best].text) {
        memcpy(s_cache[best].text, text, len);
        s_cache[best].text[len] = '\0';
        s_cache[best].hash = hash;
        s_cache[best].ts_us = esp_timer_get_time();
    }
}

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
        bool is_tavily = (strncmp(s_search_key, "tvly-", 5) == 0);
        ESP_LOGI(TAG, "Web search initialized (%s)", is_tavily ? "Tavily" : "Brave");
    } else {
        ESP_LOGW(TAG, "No search API key. Add search_key to SERVICES.md or use `set_search_key <key>`");
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

static esp_err_t search_tavily(const char *query, const char *api_key,
                               char *output, size_t output_size)
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
    if (!client) {
        free(body_str);
        free(buf);
        snprintf(output, output_size, "Error: HTTP client init failed (low memory?)");
        return ESP_FAIL;
    }
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

    int pos = 0;
    size_t remain = output_size;

    if (answer && cJSON_IsString(answer) && strlen(answer->valuestring) > 10) {
        pos += snprintf(output + pos, remain - pos, "%s\n\n", answer->valuestring);
    }
    if (results && cJSON_IsArray(results)) {
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n && i < 5 && (size_t)pos < remain - 200; i++) {
            cJSON *item  = cJSON_GetArrayItem(results, i);
            cJSON *title = cJSON_GetObjectItem(item, "title");
            cJSON *url   = cJSON_GetObjectItem(item, "url");
            cJSON *snip  = cJSON_GetObjectItem(item, "content");
            if (title && url) {
                pos += snprintf(output + pos, remain - pos, "[%d] %s\n%s\n",
                                i + 1,
                                cJSON_IsString(title) ? title->valuestring : "",
                                cJSON_IsString(url)   ? url->valuestring   : "");
            }
            if (snip && cJSON_IsString(snip) && (size_t)pos < remain - 200) {
                pos += snprintf(output + pos, remain - pos, "%.200s\n\n", snip->valuestring);
            }
        }
    }
    cJSON_Delete(root);

    if (pos == 0) return ESP_FAIL;
    return ESP_OK;
}

/* ── Brave search ─────────────────────────────────────────────── */

static esp_err_t search_brave(const char *query, const char *api_key,
                              char *output, size_t output_size)
{
    /* Brave Web Search API: GET with query params, key in header */
    char *buf = ps_malloc(MIMI_TAVILY_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    /* URL-encode query (simple: just replace spaces with +) */
    char encoded_q[256];
    size_t qi = 0;
    for (const char *p = query; *p && qi < sizeof(encoded_q) - 4; p++) {
        if (*p == ' ')      { encoded_q[qi++] = '+'; }
        else if (*p == '&') { encoded_q[qi++] = '%'; encoded_q[qi++] = '2'; encoded_q[qi++] = '6'; }
        else if (*p == '=') { encoded_q[qi++] = '%'; encoded_q[qi++] = '3'; encoded_q[qi++] = 'D'; }
        else                { encoded_q[qi++] = *p; }
    }
    encoded_q[qi] = '\0';

    char url[384];
    snprintf(url, sizeof(url),
             "https://api.search.brave.com/res/v1/web/search?q=%s&count=5", encoded_q);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        snprintf(output, output_size, "Error: HTTP client init failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", api_key);

    esp_err_t err = esp_http_client_open(client, 0);
    int total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            char errbuf[256] = {0};
            int rd = esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
            if (rd > 0) errbuf[rd] = '\0';
            ESP_LOGW(TAG, "Brave HTTP %d: %.200s", status, errbuf);
            ws_server_broadcast_monitor("error", "Brave search failed");
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
    esp_http_client_cleanup(client);

    if (err != ESP_OK || total == 0) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_ParseWithLength(buf, total);
    free(buf);
    if (!root) return ESP_FAIL;

    /* Brave response: { web: { results: [ { title, url, description } ] } } */
    cJSON *web     = cJSON_GetObjectItem(root, "web");
    cJSON *results = web ? cJSON_GetObjectItem(web, "results") : NULL;

    int pos = 0;
    size_t remain = output_size;

    if (results && cJSON_IsArray(results)) {
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n && i < 5 && (size_t)pos < remain - 200; i++) {
            cJSON *item  = cJSON_GetArrayItem(results, i);
            cJSON *title = cJSON_GetObjectItem(item, "title");
            cJSON *url_j = cJSON_GetObjectItem(item, "url");
            cJSON *desc  = cJSON_GetObjectItem(item, "description");
            if (title && url_j) {
                pos += snprintf(output + pos, remain - pos, "[%d] %s\n%s\n",
                                i + 1,
                                cJSON_IsString(title) ? title->valuestring : "",
                                cJSON_IsString(url_j) ? url_j->valuestring : "");
            }
            if (desc && cJSON_IsString(desc) && (size_t)pos < remain - 200) {
                pos += snprintf(output + pos, remain - pos, "%.200s\n\n", desc->valuestring);
            }
        }
    }
    cJSON_Delete(root);

    if (pos == 0) return ESP_FAIL;
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size,
                 "Search not available: no search API key configured. "
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

    /* Check cache before hitting the API */
    uint32_t qhash = fnv1a(query->valuestring);
    const char *cached = cache_lookup(qhash);
    if (cached) {
        ESP_LOGI(TAG, "Search cache hit (hash=%08x)", qhash);
        ws_server_broadcast_monitor_verbose("search", "cached");
        strncpy(output, cached, output_size - 1);
        output[output_size - 1] = '\0';
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* Route by key prefix: tvly- → Tavily, else → Brave */
    bool use_tavily = (strncmp(s_search_key, "tvly-", 5) == 0);
    const char *provider = use_tavily ? "Tavily" : "Brave";
    ws_server_broadcast_monitor_verbose("search", provider);

    esp_err_t err = use_tavily
        ? search_tavily(query->valuestring, s_search_key, output, output_size)
        : search_brave(query->valuestring, s_search_key, output, output_size);
    cJSON_Delete(input);

    if (err == ESP_OK) {
        s_total_searches++;
        if (use_tavily) s_search_cost_millicents += 200;
        cache_store(qhash, output);
    } else {
        snprintf(output, output_size, "Error: %s search failed", provider);
    }
    return err;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }
    nvs_commit(nvs);
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}
