#include "tool_weather.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"
#include "langoustine_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "tool_weather";

#define WEATHER_NVS_NS  "weather_config"
#define WEATHER_NVS_KEY "location"
#define WEATHER_BUF_SIZE (20 * 1024)  /* wttr.in j1 responses are typically 12-16KB */

typedef struct {
    char *data;
    int   len;
    int   cap;
} weather_resp_t;

static esp_err_t weather_event_cb(esp_http_client_event_t *evt)
{
    weather_resp_t *r = (weather_resp_t *)evt->user_data;
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

/* Read the 3 chars from hourly[4] (noon) weatherDesc, or [0] as fallback. */
static const char *hourly_desc(cJSON *weather_day)
{
    cJSON *hourly = cJSON_GetObjectItem(weather_day, "hourly");
    if (!hourly || !cJSON_IsArray(hourly)) return "unknown";
    int cnt = cJSON_GetArraySize(hourly);
    /* slot 4 = 12:00; slot 0 = 00:00 */
    int slot = (cnt > 4) ? 4 : 0;
    cJSON *h = cJSON_GetArrayItem(hourly, slot);
    if (!h) return "unknown";
    cJSON *desc_arr = cJSON_GetObjectItem(h, "weatherDesc");
    if (!desc_arr || !cJSON_IsArray(desc_arr)) return "unknown";
    cJSON *d0 = cJSON_GetArrayItem(desc_arr, 0);
    if (!d0) return "unknown";
    cJSON *v = cJSON_GetObjectItem(d0, "value");
    return (v && cJSON_IsString(v)) ? v->valuestring : "unknown";
}

esp_err_t tool_weather_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    char location[128] = {0};

    cJSON *input = cJSON_Parse(input_json);
    if (input) {
        cJSON *jloc = cJSON_GetObjectItem(input, "location");
        if (jloc && cJSON_IsString(jloc) && jloc->valuestring[0]) {
            snprintf(location, sizeof(location), "%s", jloc->valuestring);
        }
        cJSON_Delete(input);
    }

    /* Fallback to NVS default location */
    if (!location[0]) {
        nvs_handle_t nvs;
        if (nvs_open(WEATHER_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(location);
            nvs_get_str(nvs, WEATHER_NVS_KEY, location, &len);
            nvs_close(nvs);
        }
    }

    /* Build URL — empty location means wttr.in auto-detects from IP */
    char url[256];
    if (location[0]) {
        /* URL-encode spaces as + (wttr.in accepts this) */
        char encoded[128] = {0};
        int ei = 0;
        for (int i = 0; location[i] && ei < (int)sizeof(encoded) - 4; i++) {
            if (location[i] == ' ') {
                encoded[ei++] = '+';
            } else {
                encoded[ei++] = location[i];
            }
        }
        snprintf(url, sizeof(url), "https://wttr.in/%s?format=j1", encoded);
    } else {
        snprintf(url, sizeof(url), "https://wttr.in/?format=j1");
    }

    /* Allocate response buffer in PSRAM (>4KB) */
    weather_resp_t resp = {0};
    resp.data = ps_malloc(WEATHER_BUF_SIZE);
    if (!resp.data) {
        snprintf(output, output_size, "Error: out of PSRAM");
        return ESP_ERR_NO_MEM;
    }
    resp.data[0] = '\0';
    resp.cap = WEATHER_BUF_SIZE;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = weather_event_cb,
        .user_data         = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp.data);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        free(resp.data);
        snprintf(output, output_size, "Error: wttr.in request failed (HTTP %d, %s)",
                 status, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wttr.in %s → HTTP %d (%d bytes)", url, status, resp.len);
    ws_server_broadcast_monitor_verbose("tool", "get_weather → ok");

    /* Parse JSON */
    cJSON *root = cJSON_Parse(resp.data);
    free(resp.data);

    if (!root) {
        snprintf(output, output_size, "Error: failed to parse weather JSON");
        return ESP_FAIL;
    }

    /* current_condition[0] */
    cJSON *cc_arr = cJSON_GetObjectItem(root, "current_condition");
    cJSON *cc = (cc_arr && cJSON_IsArray(cc_arr)) ? cJSON_GetArrayItem(cc_arr, 0) : NULL;

    const char *desc     = "unknown";
    int         temp_c   = 0;
    int         feels_c  = 0;
    int         humidity = 0;
    int         wind_kmh = 0;
    const char *wind_dir = "";

    if (cc) {
        cJSON *j;
        cJSON *desc_arr = cJSON_GetObjectItem(cc, "weatherDesc");
        if (desc_arr && cJSON_IsArray(desc_arr)) {
            cJSON *d0 = cJSON_GetArrayItem(desc_arr, 0);
            if (d0) {
                j = cJSON_GetObjectItem(d0, "value");
                if (j && cJSON_IsString(j)) desc = j->valuestring;
            }
        }
        j = cJSON_GetObjectItem(cc, "temp_C");
        if (j && cJSON_IsString(j)) temp_c = atoi(j->valuestring);
        j = cJSON_GetObjectItem(cc, "FeelsLikeC");
        if (j && cJSON_IsString(j)) feels_c = atoi(j->valuestring);
        j = cJSON_GetObjectItem(cc, "humidity");
        if (j && cJSON_IsString(j)) humidity = atoi(j->valuestring);
        j = cJSON_GetObjectItem(cc, "windspeedKmph");
        if (j && cJSON_IsString(j)) wind_kmh = atoi(j->valuestring);
        j = cJSON_GetObjectItem(cc, "winddir16Point");
        if (j && cJSON_IsString(j)) wind_dir = j->valuestring;
    }

    int temp_f   = temp_c  * 9 / 5 + 32;
    int feels_f  = feels_c * 9 / 5 + 32;

    /* 3-day forecast */
    cJSON *weather_arr = cJSON_GetObjectItem(root, "weather");
    char forecast[256] = {0};
    int fpos = 0;
    if (weather_arr && cJSON_IsArray(weather_arr)) {
        const char *day_names[] = {"Today", "Tomorrow", "Day 3"};
        for (int i = 0; i < 3 && i < cJSON_GetArraySize(weather_arr); i++) {
            cJSON *day = cJSON_GetArrayItem(weather_arr, i);
            if (!day) continue;
            int lo = 0, hi = 0;
            cJSON *j;
            j = cJSON_GetObjectItem(day, "mintempC");
            if (j && cJSON_IsString(j)) lo = atoi(j->valuestring);
            j = cJSON_GetObjectItem(day, "maxtempC");
            if (j && cJSON_IsString(j)) hi = atoi(j->valuestring);
            const char *fdesc = hourly_desc(day);
            int lo_f = lo * 9 / 5 + 32;
            int hi_f = hi * 9 / 5 + 32;
            fpos += snprintf(forecast + fpos, sizeof(forecast) - fpos,
                             "%s%s: %d-%d°C (%d-%d°F) %s",
                             i > 0 ? "; " : "", day_names[i],
                             lo, hi, lo_f, hi_f, fdesc);
            if (fpos >= (int)sizeof(forecast) - 1) break;
        }
    }

    /* Determine display location name from nearest area */
    char loc_display[128] = {0};
    cJSON *nearest = cJSON_GetObjectItem(root, "nearest_area");
    if (nearest && cJSON_IsArray(nearest)) {
        cJSON *a0 = cJSON_GetArrayItem(nearest, 0);
        if (a0) {
            cJSON *area_name = cJSON_GetObjectItem(a0, "areaName");
            if (area_name && cJSON_IsArray(area_name)) {
                cJSON *v = cJSON_GetArrayItem(area_name, 0);
                if (v) {
                    cJSON *val = cJSON_GetObjectItem(v, "value");
                    if (val && cJSON_IsString(val))
                        snprintf(loc_display, sizeof(loc_display), "%s", val->valuestring);
                }
            }
        }
    }
    if (!loc_display[0] && location[0]) {
        snprintf(loc_display, sizeof(loc_display), "%s", location);
    }

    cJSON_Delete(root);

    /* Format output */
    if (loc_display[0]) {
        snprintf(output, output_size,
                 "%s: %s, %d°C (%d°F), feels like %d°C (%d°F). "
                 "Humidity %d%%, wind %d km/h %s. "
                 "Forecast — %s.",
                 loc_display, desc, temp_c, temp_f, feels_c, feels_f,
                 humidity, wind_kmh, wind_dir,
                 forecast[0] ? forecast : "unavailable");
    } else {
        snprintf(output, output_size,
                 "%s, %d°C (%d°F), feels like %d°C (%d°F). "
                 "Humidity %d%%, wind %d km/h %s. "
                 "Forecast — %s.",
                 desc, temp_c, temp_f, feels_c, feels_f,
                 humidity, wind_kmh, wind_dir,
                 forecast[0] ? forecast : "unavailable");
    }

    return ESP_OK;
}
