#include "ambient_monitor.h"
#include "tools/lan_request.h"
#include "config/services_parser.h"
#include "display/oled_display.h"
#include "memory/psram_alloc.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ambient";

#define KLIPPER_POLL_MS  (20 * 1000)
#define SONOS_POLL_MS    (15 * 1000)
#define STOCK_POLL_MS    (5 * 60 * 1000)   /* 5 minutes */
#define RESP_BUF         4096

/* ── Credential helpers ──────────────────────────────────────── */

typedef struct { char url[128]; char apikey[128]; } klipper_creds_t;
typedef struct { char url[128]; char token[256];  } ha_creds_t;

static bool load_klipper_creds(klipper_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    services_kv_t kv[] = {
        { "moonraker_url",    c->url,    sizeof(c->url)    },
        { "moonraker_apikey", c->apikey, sizeof(c->apikey) },
    };
    services_parse_section("## Klipper / Moonraker", kv, 2);
    return c->url[0] != '\0';
}

static bool load_ha_creds(ha_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    services_kv_t kv[] = {
        { "ha_url",   c->url,   sizeof(c->url)   },
        { "ha_token", c->token, sizeof(c->token) },
    };
    services_parse_section("## Home Assistant", kv, 2);
    return c->url[0] != '\0' && c->token[0] != '\0';
}

static void load_sonos_entity(char *out, size_t out_size)
{
    services_kv_t kv[] = { { "sonos_player", out, out_size } };
    services_parse_section("## Home Assistant", kv, 1);
    if (!out[0]) {
        strncpy(out, "media_player.living_room", out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* ── ARM stock parser ─────────────────────────────────────────── */

void ambient_parse_arm_stock(const char *content, char *out, size_t out_size)
{
    char price[16] = "";
    char move[12]  = "";

    /* Scan for first $DDD.DD pattern */
    for (const char *p = content; *p; p++) {
        if (*p == '$' && p[1] >= '0' && p[1] <= '9') {
            int i = 0;
            p++;
            while ((*p >= '0' && *p <= '9') || (*p == '.' && i < 8)) {
                if (i < (int)sizeof(price) - 1) price[i++] = *p;
                p++;
            }
            price[i] = '\0';
            /* Truncate to 2 decimal places */
            char *dot = strchr(price, '.');
            if (dot && strlen(dot) > 3) dot[3] = '\0';
            break;
        }
    }

    /* Scan for first [+-]D...% pattern */
    for (const char *p = content; *p; p++) {
        if ((*p == '+' || *p == '-') && p[1] >= '0' && p[1] <= '9') {
            int i = 0;
            char tmp[12] = "";
            if (i < (int)sizeof(tmp) - 1) tmp[i++] = *p;
            p++;
            while ((*p >= '0' && *p <= '9') || *p == '.') {
                if (i < (int)sizeof(tmp) - 1) tmp[i++] = *p;
                p++;
            }
            if (*p == '%') {
                if (i < (int)sizeof(tmp) - 1) tmp[i++] = '%';
                tmp[i] = '\0';
                strncpy(move, tmp, sizeof(move) - 1);
                break;
            }
        }
    }

    if (price[0] && move[0]) {
        snprintf(out, out_size, "ARM $%s %s", price, move);
    } else if (price[0]) {
        snprintf(out, out_size, "ARM $%s", price);
    } else {
        /* Fall back to first line of content, truncated */
        int i = 0;
        const char *p = content;
        while (*p && *p != '\n' && i < (int)out_size - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
}

/* ── Klipper print progress ───────────────────────────────────── */

static void klipper_monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(30000)); /* let system settle */

    char *buf = (char *)ps_malloc(RESP_BUF);
    if (!buf) {
        ESP_LOGE(TAG, "klipper_monitor: no PSRAM");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        klipper_creds_t creds;
        if (!load_klipper_creds(&creds)) {
            vTaskDelay(pdMS_TO_TICKS(KLIPPER_POLL_MS));
            continue;
        }

        lan_result_t res;
        esp_err_t err = lan_request(
            "GET", creds.url,
            "/printer/objects/query?print_stats&display_status",
            creds.apikey[0] ? "X-Api-Key" : NULL,
            creds.apikey[0] ? creds.apikey : NULL,
            NULL, buf, RESP_BUF, &res);

        if (err == ESP_OK && res.ok) {
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
                cJSON *status = result
                    ? cJSON_GetObjectItemCaseSensitive(result, "status") : NULL;
                cJSON *ps = status
                    ? cJSON_GetObjectItemCaseSensitive(status, "print_stats") : NULL;
                cJSON *ds = status
                    ? cJSON_GetObjectItemCaseSensitive(status, "display_status") : NULL;

                const char *state = "";
                cJSON *state_j = ps
                    ? cJSON_GetObjectItemCaseSensitive(ps, "state") : NULL;
                if (cJSON_IsString(state_j)) state = state_j->valuestring;

                if (strcmp(state, "printing") == 0) {
                    /* progress: 0.0–1.0 from display_status */
                    cJSON *prog_j = ds
                        ? cJSON_GetObjectItemCaseSensitive(ds, "progress") : NULL;
                    double progress = cJSON_IsNumber(prog_j) ? prog_j->valuedouble : 0.0;

                    /* elapsed seconds */
                    cJSON *elapsed_j = ps
                        ? cJSON_GetObjectItemCaseSensitive(ps, "print_duration") : NULL;
                    double elapsed = cJSON_IsNumber(elapsed_j) ? elapsed_j->valuedouble : 0.0;

                    /* filename — basename only */
                    cJSON *file_j = ps
                        ? cJSON_GetObjectItemCaseSensitive(ps, "filename") : NULL;
                    const char *path = cJSON_IsString(file_j) ? file_j->valuestring : "";
                    const char *fname = strrchr(path, '/');
                    fname = fname ? fname + 1 : path;

                    /* ETA */
                    int eta_mins = -1;
                    if (progress > 0.005 && elapsed > 0) {
                        double remaining = (elapsed / progress) - elapsed;
                        if (remaining > 0) eta_mins = (int)(remaining / 60.0);
                    }

                    oled_display_set_print_progress(
                        (int)(progress * 100.0), eta_mins, fname);
                } else {
                    oled_display_set_print_progress(-1, 0, NULL);
                }
                cJSON_Delete(root);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(KLIPPER_POLL_MS));
    }
}

/* ── Sonos now-playing ─────────────────────────────────────────── */

static void sonos_monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(20000)); /* let system settle */

    char *buf = (char *)ps_malloc(RESP_BUF);
    if (!buf) {
        ESP_LOGE(TAG, "sonos_monitor: no PSRAM");
        vTaskDelete(NULL);
        return;
    }

    char entity[80];
    char endpoint[128];
    char auth_hdr[300];

    for (;;) {
        ha_creds_t creds;
        if (!load_ha_creds(&creds)) {
            vTaskDelay(pdMS_TO_TICKS(SONOS_POLL_MS));
            continue;
        }

        load_sonos_entity(entity, sizeof(entity));
        snprintf(endpoint, sizeof(endpoint), "/api/states/%s", entity);
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", creds.token);

        lan_result_t res;
        esp_err_t err = lan_request(
            "GET", creds.url, endpoint,
            "Authorization", auth_hdr,
            NULL, buf, RESP_BUF, &res);

        if (err == ESP_OK && res.ok) {
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *state_j = cJSON_GetObjectItemCaseSensitive(root, "state");
                const char *state = cJSON_IsString(state_j)
                    ? state_j->valuestring : "";

                if (strcmp(state, "playing") == 0) {
                    cJSON *attrs = cJSON_GetObjectItemCaseSensitive(root, "attributes");
                    cJSON *artist_j = attrs
                        ? cJSON_GetObjectItemCaseSensitive(attrs, "media_artist") : NULL;
                    cJSON *title_j  = attrs
                        ? cJSON_GetObjectItemCaseSensitive(attrs, "media_title")  : NULL;

                    const char *artist = cJSON_IsString(artist_j)
                        ? artist_j->valuestring : "";
                    const char *title  = cJSON_IsString(title_j)
                        ? title_j->valuestring  : "";

                    char line[22] = "";
                    if (artist[0] && title[0]) {
                        /* ">Artist-Title" — 10 chars each, truncated */
                        snprintf(line, sizeof(line), ">%.9s %.9s", artist, title);
                    } else if (title[0]) {
                        snprintf(line, sizeof(line), ">%.19s", title);
                    } else if (artist[0]) {
                        snprintf(line, sizeof(line), ">%.19s", artist);
                    }
                    oled_display_set_rotate_line(5, line);
                } else {
                    /* paused / idle / off — clear the slot */
                    oled_display_set_rotate_line(5, "");
                }
                cJSON_Delete(root);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SONOS_POLL_MS));
    }
}

/* ── ARM stock live fetch ──────────────────────────────────────── */

/* Extract a JSON number field from a (possibly truncated) buffer.
 * Scans for "key": and reads the double that follows. Returns -1 on miss. */
static double json_extract_double(const char *buf, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1.0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return strtod(p, NULL);
}

static void arm_stock_live_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000)); /* stagger start */

    char buf[256];  /* response is ~60 bytes */

    for (;;) {
        lan_result_t res;
        /* Poll the Mac-side arm_stock_server.py rather than Yahoo directly.
         * Mac handles the Yahoo fetch + caching; ESP32 gets clean LAN JSON. */
        esp_err_t err = lan_request(
            "GET",
            "http://192.168.0.51:11437",
            "/arm_stock",
            NULL, NULL,
            NULL, buf, sizeof(buf), &res);

        if (err == ESP_OK && res.ok) {
            double price      = json_extract_double(buf, "price");
            double change_pct = json_extract_double(buf, "change_pct");

            if (price > 0) {
                char line[22];
                double abs_c = change_pct < 0 ? -change_pct : change_pct;
                char sign    = change_pct >= 0 ? '+' : '-';
                int abs_i    = (int)(abs_c * 10);
                int p4       = (int)price % 10000;
                int ci       = (abs_i > 99) ? 99 : abs_i;
                char chg[8];
                chg[0] = sign;
                chg[1] = (char)('0' + (ci / 10));
                chg[2] = '.';
                chg[3] = (char)('0' + (ci % 10));
                chg[4] = '%';
                chg[5] = '\0';
                snprintf(line, sizeof(line), "ARM $%d %s", p4, chg);
                oled_display_set_arm_header(line);
                ESP_LOGI(TAG, "ARM: %s", line);
            }
        } else {
            ESP_LOGW(TAG, "ARM fetch failed err=%d http=%d", err, res.http_status);
        }

        vTaskDelay(pdMS_TO_TICKS(STOCK_POLL_MS));
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void ambient_monitor_start(void)
{
    xTaskCreate(klipper_monitor_task, "klipper_mon", 4096, NULL, 2, NULL);
    xTaskCreate(sonos_monitor_task,   "sonos_mon",   4096, NULL, 2, NULL);
    xTaskCreate(arm_stock_live_task,  "arm_stock",   6144, NULL, 2, NULL);
    ESP_LOGI(TAG, "Ambient monitor tasks started");
}
