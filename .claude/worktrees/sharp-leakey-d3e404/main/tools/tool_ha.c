#include "tool_ha.h"
#include "lan_request.h"
#include "config/services_parser.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

static const char *TAG = "tool_ha";

/* Max bytes of HA response body returned to the model */
#define HA_BODY_MAX     2048

/* ── SERVICES.md credentials ─────────────────────────────────── */

typedef struct {
    char url[128];    /* e.g. "http://192.168.0.50:8123" */
    char token[256];  /* Long-Lived Access Token */
} ha_creds_t;

static bool parse_ha_creds(ha_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    services_kv_t kvs[] = {
        { "ha_url",   c->url,   sizeof(c->url)   },
        { "ha_token", c->token, sizeof(c->token) },
    };
    services_parse_section("## Home Assistant", kvs, 2);
    return c->url[0] != '\0' && c->token[0] != '\0';
}

/* ── Endpoint blocking ───────────────────────────────────────── */

/* Returns true if the endpoint must be blocked.
 * - /api/config*          blocked (exposes HA secrets)
 * - /api/services/hassio* blocked (supervisor / add-on access)
 * - /api/states exactly   blocked (bulk dump — heap risk)
 * - /api/states/...       ALLOWED (specific entity endpoint)
 */
static bool endpoint_blocked(const char *ep)
{
    static const char * const BLOCKED_PREFIXES[] = {
        "/api/config",
        "/api/services/hassio",
        NULL
    };
    for (int i = 0; BLOCKED_PREFIXES[i]; i++) {
        if (strncmp(ep, BLOCKED_PREFIXES[i],
                    strlen(BLOCKED_PREFIXES[i])) == 0) {
            return true;
        }
    }
    /* Exact /api/states blocked; /api/states/<entity> is fine */
    if (strcmp(ep, "/api/states") == 0) return true;
    return false;
}

/* ── Helper: write structured JSON error into output ─────────── */

static void ha_err(char *out, size_t out_size, int status,
                   const char *error, const char *reason)
{
    snprintf(out, out_size,
             "{\"ok\":false,\"status\":%d,\"error\":\"%s\","
             "\"reason\":\"%s\",\"bytes\":0}",
             status, error, reason);
}

/* ── Tool entry point ────────────────────────────────────────── */

esp_err_t tool_ha_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        ha_err(output, output_size, 0, "parse_error", "Invalid JSON input");
        return ESP_OK;
    }

    char method[16]   = "GET";
    char endpoint[256] = {0};
    char body_str[512] = {0};

    cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *je = cJSON_GetObjectItemCaseSensitive(root, "endpoint");
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "body");

    if (cJSON_IsString(jm) && jm->valuestring[0]) snprintf(method,   sizeof(method),   "%s", jm->valuestring);
    if (cJSON_IsString(je))                        snprintf(endpoint, sizeof(endpoint), "%s", je->valuestring);
    if (cJSON_IsString(jb) && jb->valuestring[0]) snprintf(body_str, sizeof(body_str), "%s", jb->valuestring);

    if (endpoint[0] == '\0') {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "missing_endpoint", "endpoint is required");
        return ESP_OK;
    }

    if (strncmp(endpoint, "/api/", 5) != 0) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "invalid_endpoint",
               "endpoint must start with /api/");
        return ESP_OK;
    }

    if (endpoint_blocked(endpoint)) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "blocked_endpoint",
               "endpoint not permitted for security reasons");
        ESP_LOGW(TAG, "Blocked HA endpoint: %s", endpoint);
        return ESP_OK;
    }

    /* Load credentials */
    ha_creds_t creds;
    if (!parse_ha_creds(&creds)) {
        cJSON_Delete(root);
        ha_err(output, output_size, 0, "no_credentials",
               "SERVICES.md missing ## Home Assistant section with ha_url and ha_token");
        return ESP_OK;
    }

    /* Build Authorization header value: "Bearer <token>" */
    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", creds.token);
    memset(creds.token, 0, sizeof(creds.token));  /* zero token — now in auth_header */

    /* Allocate response buffer */
    char *resp_buf = ps_malloc(HA_BODY_MAX + 1);
    if (!resp_buf) {
        cJSON_Delete(root);
        memset(auth_header, 0, sizeof(auth_header));
        ha_err(output, output_size, 0, "no_mem", "ESP_ERR_NO_MEM");
        return ESP_OK;
    }

    lan_result_t result;
    lan_request(method, creds.url, endpoint,
                "Authorization", auth_header,
                body_str,
                resp_buf, HA_BODY_MAX + 1,
                &result);

    cJSON_Delete(root);
    memset(auth_header, 0, sizeof(auth_header));

    lan_result_to_json(&result, resp_buf, output, output_size);
    free(resp_buf);

    {
        char mon[320];
        snprintf(mon, sizeof(mon), "%s %.256s → %d (%d bytes%s)",
                 method, endpoint, result.http_status, result.bytes,
                 result.truncated ? ", truncated" : "");
        ws_server_broadcast_monitor("ha", mon);
    }
    ESP_LOGI(TAG, "HA %s %s → %d (%d bytes%s)",
             method, endpoint, result.http_status, result.bytes,
             result.truncated ? " truncated" : "");

    return ESP_OK;
}
