#include "tool_noaa_buoy.h"
#include "memory/psram_alloc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "noaa_buoy";

#define NOAA_BUF_SIZE       (8 * 1024)
#define NOAA_DEFAULT_STATION "46012"
#define NOAA_STATION_MAX    16

/* NDBC realtime2 endpoint — plain text, no API key */
#define NOAA_URL_FMT "https://www.ndbc.noaa.gov/data/realtime2/%s.txt"

/* ── HTTP response buffer ───────────────────────────────────── */

typedef struct {
    char *data;
    int   len;
    int   cap;
} noaa_buf_t;

static esp_err_t noaa_http_event_cb(esp_http_client_event_t *evt)
{
    noaa_buf_t *r = (noaa_buf_t *)evt->user_data;
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

/* ── Compass direction from degrees ─────────────────────────── */

static const char *deg_to_compass(int deg)
{
    /* 16-point compass */
    static const char *pts[] = {
        "N","NNE","NE","ENE","E","ESE","SE","SSE",
        "S","SSW","SW","WSW","W","WNW","NW","NNW"
    };
    int idx = (int)(((float)deg + 11.25f) / 22.5f) % 16;
    return pts[idx];
}

/* ── Parse NDBC realtime2 text format ───────────────────────── */
/*
 * Header lines start with '#'. First data row is the most recent observation.
 * Column order (space-separated, MM = missing):
 *  #YY  MM DD hh mm WDIR WSPD GST  WVHT   DPD   APD MWD   PRES  ATMP  WTMP  DEWP  VIS PTDY  TIDE
 *   0    1  2  3  4    5    6   7     8     9    10  11    12    13    14    15   16   17    18
 */
typedef struct {
    int   year, month, day, hour, min;
    float wdir_deg;     /* wind direction deg true */
    float wspd_ms;      /* wind speed m/s          */
    float wvht_m;       /* significant wave height m */
    float dpd_s;        /* dominant period s         */
    float mwd_deg;      /* mean wave direction deg   */
    float wtmp_c;       /* water temp C              */
    bool  wvht_valid;
    bool  dpd_valid;
    bool  mwd_valid;
    bool  wspd_valid;
    bool  wdir_valid;
    bool  wtmp_valid;
} noaa_obs_t;

static bool parse_float_field(const char *s, float *out)
{
    if (!s || strcmp(s, "MM") == 0 || strcmp(s, "99.00") == 0 || strcmp(s, "999") == 0) {
        return false;
    }
    char *end;
    float v = strtof(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

/* Tokenise a single space-delimited line into fields array.
   Returns number of fields found. Fields point into (modified) line_buf. */
static int tokenise_line(char *line_buf, char **fields, int max_fields)
{
    int n = 0;
    char *p = line_buf;
    while (*p && n < max_fields) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        fields[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

static bool parse_ndbc_text(const char *text, noaa_obs_t *obs)
{
    memset(obs, 0, sizeof(*obs));

    /* Walk lines looking for first non-comment data row */
    const char *line_start = text;
    bool header_passed = false;

    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        /* Copy line to mutable buffer */
        char line_buf[256];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';

        if (line_buf[0] == '#') {
            /* Second header line (units) starts with "#yr" — skip both */
            header_passed = true;
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }

        if (!header_passed) {
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }

        /* First non-comment line = most recent obs */
        char *fields[24];
        int n = tokenise_line(line_buf, fields, 24);
        if (n < 15) {
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }

        obs->year  = atoi(fields[0]);
        obs->month = atoi(fields[1]);
        obs->day   = atoi(fields[2]);
        obs->hour  = atoi(fields[3]);
        obs->min   = atoi(fields[4]);

        float v;
        if (parse_float_field(fields[5], &v))  { obs->wdir_deg = v; obs->wdir_valid = true; }
        if (parse_float_field(fields[6], &v))  { obs->wspd_ms  = v; obs->wspd_valid = true; }
        if (parse_float_field(fields[8], &v))  { obs->wvht_m   = v; obs->wvht_valid = true; }
        if (parse_float_field(fields[9], &v))  { obs->dpd_s    = v; obs->dpd_valid  = true; }
        if (n > 11 && parse_float_field(fields[11], &v)) { obs->mwd_deg = v; obs->mwd_valid = true; }
        if (n > 14 && parse_float_field(fields[14], &v)) { obs->wtmp_c  = v; obs->wtmp_valid = true; }

        return true;
    }
    return false;
}

/* ── Unit conversions ───────────────────────────────────────── */

static float m_to_ft(float m)   { return m * 3.28084f; }
static float ms_to_mph(float ms) { return ms * 2.23694f; }
static float c_to_f(float c)    { return c * 9.0f / 5.0f + 32.0f; }

/* ── Main execute ───────────────────────────────────────────── */

esp_err_t tool_noaa_buoy_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse station from input */
    char station[NOAA_STATION_MAX] = NOAA_DEFAULT_STATION;

    if (input_json && input_json[0]) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *st = cJSON_GetObjectItem(root, "station");
            if (st && cJSON_IsString(st) && st->valuestring[0]) {
                strncpy(station, st->valuestring, sizeof(station) - 1);
                station[sizeof(station) - 1] = '\0';
            }
            cJSON_Delete(root);
        }
    }

    /* Build URL */
    char url[128];
    snprintf(url, sizeof(url), NOAA_URL_FMT, station);
    ESP_LOGI(TAG, "Fetching NOAA buoy %s: %s", station, url);

    /* Allocate response buffer from PSRAM */
    noaa_buf_t buf = {
        .data = (char *)ps_malloc(NOAA_BUF_SIZE),
        .len  = 0,
        .cap  = NOAA_BUF_SIZE,
    };
    if (!buf.data) {
        snprintf(output, output_size, "Error: out of memory allocating buoy buffer");
        return ESP_ERR_NO_MEM;
    }
    buf.data[0] = '\0';

    /* HTTP GET */
    esp_http_client_config_t cfg = {
        .url              = url,
        .timeout_ms       = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler    = noaa_http_event_cb,
        .user_data        = &buf,
        .buffer_size      = 1024,
        .buffer_size_tx   = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf.data);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        free(buf.data);
        snprintf(output, output_size, "Error: HTTP request failed (%s)", esp_err_to_name(ret));
        return ret;
    }
    if (status != 200) {
        free(buf.data);
        snprintf(output, output_size,
                 "Error: NOAA returned HTTP %d for station %s. "
                 "Check station ID at https://www.ndbc.noaa.gov/", status, station);
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse the NDBC text */
    noaa_obs_t obs;
    bool ok = parse_ndbc_text(buf.data, &obs);
    free(buf.data);

    if (!ok) {
        snprintf(output, output_size,
                 "Error: could not parse NOAA data for station %s. "
                 "Station may be offline or not reporting wave data.", station);
        return ESP_FAIL;
    }

    /* Station friendly names for known stations */
    const char *station_name = "Unknown";
    if (strcmp(station, "46012") == 0)      station_name = "Point Reyes (NW of SF/Pacifica)";
    else if (strcmp(station, "46026") == 0) station_name = "San Francisco Bar";
    else if (strcmp(station, "46013") == 0) station_name = "Bodega Bay";
    else if (strcmp(station, "46214") == 0) station_name = "Point Arena";

    /* Format output */
    int pos = 0;
    pos += snprintf(output + pos, output_size - pos,
                    "NOAA Buoy %s — %s\n"
                    "Observation: %04d-%02d-%02d %02d:%02dZ\n\n",
                    station, station_name,
                    obs.year, obs.month, obs.day, obs.hour, obs.min);

    if (obs.wvht_valid) {
        pos += snprintf(output + pos, output_size - pos,
                        "Wave height:  %.1f ft (%.1f m)\n",
                        m_to_ft(obs.wvht_m), obs.wvht_m);
    } else {
        pos += snprintf(output + pos, output_size - pos, "Wave height:  not reported\n");
    }

    if (obs.dpd_valid) {
        pos += snprintf(output + pos, output_size - pos,
                        "Dominant period: %.0f s\n", obs.dpd_s);
    } else {
        pos += snprintf(output + pos, output_size - pos, "Dominant period: not reported\n");
    }

    if (obs.mwd_valid) {
        pos += snprintf(output + pos, output_size - pos,
                        "Swell direction: %s (%.0f°)\n",
                        deg_to_compass((int)obs.mwd_deg), obs.mwd_deg);
    } else if (obs.wdir_valid) {
        pos += snprintf(output + pos, output_size - pos,
                        "Swell direction: not reported (wind from %s)\n",
                        deg_to_compass((int)obs.wdir_deg));
    }

    if (obs.wspd_valid) {
        const char *wdir_str = obs.wdir_valid ? deg_to_compass((int)obs.wdir_deg) : "?";
        pos += snprintf(output + pos, output_size - pos,
                        "Wind:         %s at %.0f mph (%.1f m/s)\n",
                        wdir_str, ms_to_mph(obs.wspd_ms), obs.wspd_ms);
    } else {
        pos += snprintf(output + pos, output_size - pos, "Wind:         not reported\n");
    }

    if (obs.wtmp_valid) {
        pos += snprintf(output + pos, output_size - pos,
                        "Water temp:   %.0f°F (%.1f°C)\n",
                        c_to_f(obs.wtmp_c), obs.wtmp_c);
    }

    /* Surf quality hint */
    if (obs.wvht_valid && obs.dpd_valid) {
        float ht_ft = m_to_ft(obs.wvht_m);
        float wind_mph = obs.wspd_valid ? ms_to_mph(obs.wspd_ms) : 0.0f;

        /* Rough onshore check: wind from W/SW/NW = onshore at Pacifica */
        bool onshore = false;
        if (obs.wdir_valid) {
            int wd = (int)obs.wdir_deg;
            /* W=270, SW=225, NW=315 — onshore if 200–340 deg */
            onshore = (wd >= 200 && wd <= 340);
        }

        const char *verdict;
        if (ht_ft >= 3.0f && obs.dpd_s >= 10.0f && (!onshore || wind_mph < 5.0f)) {
            verdict = "Conditions look PROMISING for Pacifica/Lindamar. Verify with live cam.";
        } else if (ht_ft >= 2.0f && obs.dpd_s >= 10.0f && wind_mph < 15.0f) {
            verdict = "Marginal — could be rideable, check live cam before driving.";
        } else if (ht_ft < 2.0f) {
            verdict = "Too small for Pacifica. Probably not worth the drive.";
        } else if (onshore && wind_mph > 10.0f) {
            verdict = "Strong onshore wind likely blowing it out.";
        } else {
            verdict = "Mixed signals — check surf-forecast skill for full assessment.";
        }

        pos += snprintf(output + pos, output_size - pos,
                        "\nQuick surf hint (Pacifica/Lindamar):\n%s\n", verdict);
    }

    pos += snprintf(output + pos, output_size - pos,
                    "\nSource: NOAA NDBC realtime2 (buoy observation, not forecast)\n"
                    "Note: Buoy is offshore — nearshore conditions may differ. "
                    "Pair with surf-forecast skill for full verdict.");

    ESP_LOGI(TAG, "Buoy %s: wvht=%.2fm dpd=%.0fs wspd=%.1fm/s",
             station,
             obs.wvht_valid ? obs.wvht_m : 0.0f,
             obs.dpd_valid  ? obs.dpd_s  : 0.0f,
             obs.wspd_valid ? obs.wspd_ms : 0.0f);

    return ESP_OK;
}
