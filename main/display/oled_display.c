#include "display/oled_display.h"
#include "display/ssd1306.h"
#include "langoustine_config.h"
#include "led/led_indicator.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

/* ── Thread-safe state (written by any task, read by display task) ── */

static char s_status[32]  = "Booting";
static char s_message[128] = "";
static TickType_t s_message_set_at = 0;
#define MSG_AUTO_CLEAR_TICKS  pdMS_TO_TICKS(30000)  /* auto-clear after 30s */
static char s_alert1[32]  = "";
static char s_alert2[32]  = "";
static TickType_t s_alert_until = 0;
static char s_provider_info[32] = "";
static char s_token_info[32]    = "";
static char s_ip_addr[20]       = "";
static bool s_ollama_online     = false;
static bool s_audio_online      = false;
static bool s_apfel_online      = false;
static char s_channel[8]        = "";
static int  s_msg_count_today   = 0;
static int  s_msg_count_yday    = -1;
static char s_rotate_lines[6][22] = { "", "", "", "", "", "" };
static int  s_ota_pct            = -1;  /* <0 = no OTA screen */
static char s_ota_label[16]      = "";
/* print progress: -1 = not printing */
static int  s_print_pct          = -1;
static int  s_print_eta_mins     = -1;
static char s_print_fname[22]    = "";
/* ARM stock header (top-right, always visible) */
static char s_arm_hdr[22]        = "";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── WiFi RSSI helper ────────────────────────────────────────────── */

static int get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return -100;
}

/* Draw RSSI bar (4 bars, 20px wide) */
static void draw_rssi(int x, int y, int rssi)
{
    /* Map RSSI to 0-4 bars: >-50=4, >-60=3, >-70=2, >-80=1, else 0 */
    int bars = 0;
    if      (rssi > -50) bars = 4;
    else if (rssi > -60) bars = 3;
    else if (rssi > -70) bars = 2;
    else if (rssi > -80) bars = 1;

    for (int b = 0; b < 4; b++) {
        int bh = 2 + b * 2;  /* bar heights: 2, 4, 6, 8 */
        int bx = x + b * 5;
        int by = y + (8 - bh);
        ssd1306_fill_rect(bx, by, 3, bh, b < bars);
    }
}

/* ── Display screens ─────────────────────────────────────────────── */

/* ── Diagnostics screen (SRAM / uptime / IP — shown periodically) ── */
static void draw_diag_screen(void)
{
    portENTER_CRITICAL(&s_mux);
    char ip_copy[20];
    strncpy(ip_copy, s_ip_addr, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';
    char tok_copy[32];
    strncpy(tok_copy, s_token_info, sizeof(tok_copy) - 1);
    tok_copy[sizeof(tok_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    ssd1306_str(0, 0,  "-- diagnostics --");

    uint32_t sram  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char stat[22];
    snprintf(stat, sizeof(stat), "S:%luK  P:%luM",
             (unsigned long)(sram / 1024), (unsigned long)(psram / (1024 * 1024)));
    ssd1306_str(0, 10, stat);

    if (tok_copy[0]) ssd1306_str(0, 20, tok_copy);

    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    int hrs  = (int)(uptime_s / 3600);
    int mins = (int)((uptime_s % 3600) / 60);
    char up[22];
    snprintf(up, sizeof(up), "Up: %dh %02dm", hrs, mins);
    ssd1306_str(0, 30, up);

    if (ip_copy[0]) ssd1306_str(0, 40, ip_copy);

    int rssi = get_rssi();
    char rssi_str[22];
    snprintf(rssi_str, sizeof(rssi_str), "WiFi: %d dBm", rssi);
    ssd1306_str(0, 50, rssi_str);
}

/* ── Main idle screen ─────────────────────────────────────────────── */
static void draw_idle_screen(void)
{
    /* Every 2 min show diagnostics for 5s (10 renders at 2Hz) */
    static int render_tick = 0;
    render_tick++;
    if ((render_tick % 240) >= 230) {
        draw_diag_screen();
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int rssi = get_rssi();

    /* ── Row 0-15: Large HH:MM (left) + service status + RSSI (right) */
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    ssd1306_str_2x(0, 0, time_str);

    portENTER_CRITICAL(&s_mux);
    char arm_hdr[22];
    strncpy(arm_hdr, s_arm_hdr, sizeof(arm_hdr) - 1);
    arm_hdr[sizeof(arm_hdr) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    /* Top-right: ARM price row 0, RSSI bars row 8 */
    if (arm_hdr[0]) ssd1306_str(64, 0, arm_hdr);
    draw_rssi(108, 8, rssi);

    /* ── Row 18: date ────────────────────────────────────────────── */
    static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    char date_str[22];
    snprintf(date_str, sizeof(date_str), "%s %s %d",
             days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday);
    ssd1306_str(0, 18, date_str);

    /* Separator */
    ssd1306_hline(0, 28, 128, true);

    /* ── Rows 32 + 43: two rotating info lines or print bar ─────── */
    portENTER_CRITICAL(&s_mux);
    char msg_copy[128];
    strncpy(msg_copy, s_message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';
    int print_pct  = s_print_pct;
    int print_eta  = s_print_eta_mins;
    char fname[22];
    strncpy(fname, s_print_fname, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (msg_copy[0]) {
        /* Message preview spans both rows */
        char line[22];
        strncpy(line, msg_copy, 21); line[21] = '\0';
        ssd1306_str(0, 32, line);
        if (strlen(msg_copy) > 21) {
            strncpy(line, msg_copy + 21, 21); line[21] = '\0';
            ssd1306_str(0, 43, line);
        }
    } else if (print_pct >= 0) {
        /* Print progress bar row 32 */
        char bar[22];
        int filled = (print_pct * 10) / 100;
        bar[0] = '[';
        for (int i = 0; i < 10; i++) bar[1 + i] = (i < filled) ? '#' : '-';
        bar[11] = ']';
        int pct_d = (print_pct > 100) ? 100 : print_pct;
        char pct_str[6];
        snprintf(pct_str, sizeof(pct_str), " %d%%", pct_d);
        strncat(bar, pct_str, sizeof(bar) - 13);
        ssd1306_str(0, 32, bar);

        /* ETA + filename row 43 */
        char eta_line[22] = "";
        if (print_eta >= 0 && fname[0]) {
            int ec = (print_eta > 999) ? 999 : print_eta;
            char es[6]; snprintf(es, sizeof(es), "~%dm", ec);
            snprintf(eta_line, sizeof(eta_line), "%-5s %.14s", es, fname);
        } else if (print_eta >= 0) {
            int ec = (print_eta > 999) ? 999 : print_eta;
            snprintf(eta_line, sizeof(eta_line), "~%dm left", ec);
        } else if (fname[0]) {
            strncpy(eta_line, fname, sizeof(eta_line) - 1);
            eta_line[sizeof(eta_line) - 1] = '\0';
        }
        if (eta_line[0]) ssd1306_str(0, 43, eta_line);
    } else {
        /* Two rotating lines, 2s each, slots offset by 3 */
        static int rot_tick = 0;
        rot_tick++;
        int phaseA = (rot_tick / 4) % 6;   /* 2s per slot at 2Hz */
        int phaseB = (phaseA + 3) % 6;

        portENTER_CRITICAL(&s_mux);
        char lineA[22], lineB[22];
        const char *srcA = s_rotate_lines[phaseA][0] ? s_rotate_lines[phaseA]
                         : (s_provider_info[0] ? s_provider_info : "Ready");
        strncpy(lineA, srcA, 21); lineA[21] = '\0';
        strncpy(lineB, s_rotate_lines[phaseB], 21); lineB[21] = '\0';
        portEXIT_CRITICAL(&s_mux);

        ssd1306_str(0, 32, lineA);
        if (lineB[0]) ssd1306_str(0, 43, lineB);
    }

    /* ── Row 54: daily message count ─────────────────────────────── */
    portENTER_CRITICAL(&s_mux);
    int msgs = s_msg_count_today;
    portEXIT_CRITICAL(&s_mux);
    char summary[22];
    snprintf(summary, sizeof(summary), "%d msgs today", msgs);
    ssd1306_str(0, 54, summary);
}

static void draw_active_screen(void)
{
    /* Snapshot all state under lock */
    portENTER_CRITICAL(&s_mux);
    char prov_copy[32], status_copy[32], msg_copy[128], tok_copy[32];
    strncpy(prov_copy, s_provider_info, sizeof(prov_copy) - 1);
    prov_copy[sizeof(prov_copy) - 1] = '\0';
    strncpy(status_copy, s_status, sizeof(status_copy) - 1);
    status_copy[sizeof(status_copy) - 1] = '\0';
    strncpy(msg_copy, s_message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';
    strncpy(tok_copy, s_token_info, sizeof(tok_copy) - 1);
    tok_copy[sizeof(tok_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    /* Row 0: Provider/model */
    if (prov_copy[0]) {
        ssd1306_str(0, 0, prov_copy);
    }

    /* Separator */
    ssd1306_hline(0, 10, 128, true);

    /* Row 12-20: [channel] Status + msg count */
    portENTER_CRITICAL(&s_mux);
    char chan_copy[8];
    strncpy(chan_copy, s_channel, sizeof(chan_copy) - 1);
    chan_copy[sizeof(chan_copy) - 1] = '\0';
    int msg_cnt = s_msg_count_today;
    portEXIT_CRITICAL(&s_mux);

    if (chan_copy[0]) {
        char status_line[44];
        snprintf(status_line, sizeof(status_line), "[%s] %s", chan_copy, status_copy);
        ssd1306_str(0, 12, status_line);
    } else {
        ssd1306_str(0, 12, status_copy);
    }
    if (msg_cnt > 0) {
        char cnt[12];
        snprintf(cnt, sizeof(cnt), "#%d", msg_cnt);
        int cx = 128 - (int)strlen(cnt) * 6;
        if (cx > 0) ssd1306_str(cx, 12, cnt);
    }

    /* Separator */
    ssd1306_hline(0, 22, 128, true);

    /* Row 24-44: Message preview (up to 3 lines) */
    if (msg_copy[0]) {
        char line[22];
        strncpy(line, msg_copy, 21);
        line[21] = '\0';
        ssd1306_str(0, 24, line);

        if (strlen(msg_copy) > 21) {
            strncpy(line, msg_copy + 21, 21);
            line[21] = '\0';
            ssd1306_str(0, 32, line);
        }

        if (strlen(msg_copy) > 42) {
            strncpy(line, msg_copy + 42, 21);
            line[21] = '\0';
            ssd1306_str(0, 40, line);
        }
    }

    /* Row 48: Token counts */
    if (tok_copy[0]) {
        ssd1306_str(0, 48, tok_copy);
    }

    /* Row 56: RSSI bars + uptime */
    draw_rssi(0, 56, get_rssi());
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    int hrs = (int)(uptime_s / 3600);
    int mins = (int)((uptime_s % 3600) / 60);
    char up_line[22];
    snprintf(up_line, sizeof(up_line), "Up:%dh%02dm", hrs, mins);
    ssd1306_str(24, 56, up_line);
}

static void draw_ota_screen(void)
{
    portENTER_CRITICAL(&s_mux);
    int pct = s_ota_pct;
    char label[16];
    strncpy(label, s_ota_label, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    /* Title */
    ssd1306_str_2x(22, 2, "OTA");

    /* State label */
    ssd1306_str(0, 22, label);

    /* Percentage right-aligned on label row */
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", pct < 0 ? 0 : pct);
    int px = 128 - (int)strlen(pct_str) * 6;
    ssd1306_str(px, 22, pct_str);

    /* Progress bar: 4px border at y=34, 120px wide, 12px tall */
    int bar_x = 4, bar_y = 34, bar_w = 120, bar_h = 12;

    /* Border rectangle (draw 4 sides) */
    ssd1306_hline(bar_x, bar_y, bar_w, true);
    ssd1306_hline(bar_x, bar_y + bar_h - 1, bar_w, true);
    for (int y = bar_y; y < bar_y + bar_h; y++) {
        ssd1306_fill_rect(bar_x, y, 1, 1, true);
        ssd1306_fill_rect(bar_x + bar_w - 1, y, 1, 1, true);
    }

    /* Fill: inner area = (bar_x+2, bar_y+2, bar_w-4, bar_h-4) */
    int fill_w = (bar_w - 4) * (pct < 0 ? 0 : pct) / 100;
    if (fill_w > 0) {
        ssd1306_fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, true);
    }

    /* Uptime below bar */
    int64_t up_s = esp_timer_get_time() / 1000000LL;
    int up_h = (int)(up_s / 3600);
    int up_m = (int)((up_s % 3600) / 60);
    char bottom[22];
    snprintf(bottom, sizeof(bottom), "Up:%dh%02dm", up_h, up_m);
    ssd1306_str(0, 54, bottom);
}

static void draw_alert_screen(void)
{
    portENTER_CRITICAL(&s_mux);
    char a1[32], a2[32];
    strncpy(a1, s_alert1, sizeof(a1) - 1); a1[31] = '\0';
    strncpy(a2, s_alert2, sizeof(a2) - 1); a2[31] = '\0';
    portEXIT_CRITICAL(&s_mux);

    /* Center alert text vertically */
    ssd1306_str(0, 20, a1);
    ssd1306_str(0, 32, a2);

    /* Border */
    ssd1306_hline(0, 0, 128, true);
    ssd1306_hline(0, 63, 128, true);
}

/* ── Display refresh task ────────────────────────────────────────── */

static void oled_task(void *arg)
{
    (void)arg;

    while (1) {
        ssd1306_clear();

        /* Auto-clear stale messages after 30s */
        portENTER_CRITICAL(&s_mux);
        if (s_message[0] && s_message_set_at &&
            (xTaskGetTickCount() - s_message_set_at) > MSG_AUTO_CLEAR_TICKS) {
            s_message[0] = '\0';
        }
        portEXIT_CRITICAL(&s_mux);

        /* Check OTA screen (highest priority) */
        bool ota_active = false;
        portENTER_CRITICAL(&s_mux);
        if (s_ota_pct >= 0) ota_active = true;
        portEXIT_CRITICAL(&s_mux);

        /* Check if alert is active */
        bool alert_active = false;
        portENTER_CRITICAL(&s_mux);
        if (s_alert_until > 0 && xTaskGetTickCount() < s_alert_until) {
            alert_active = true;
        } else {
            s_alert_until = 0;
        }
        portEXIT_CRITICAL(&s_mux);

        if (ota_active) {
            draw_ota_screen();
        } else if (alert_active) {
            draw_alert_screen();
        } else {
            /* Use LED state to decide idle vs active screen */
            led_state_t led = led_indicator_get_state();
            bool active = (led == LED_THINKING || led == LED_SPEAKING ||
                           led == LED_LISTENING || led == LED_CAPTURING ||
                           led == LED_FLASH_FADE);
            if (active) {
                draw_active_screen();
            } else {
                draw_idle_screen();
            }
        }

        ssd1306_refresh();
        vTaskDelay(pdMS_TO_TICKS(500));  /* 2 Hz refresh */
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t oled_display_init(i2c_master_bus_handle_t bus)
{
#if !LANG_OLED_ENABLED
    ESP_LOGI(TAG, "OLED display disabled in config");
    return ESP_OK;
#endif

    esp_err_t ret = ssd1306_init(bus, LANG_OLED_ADDR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Boot test: fill screen white for 1 second so user can see it works */
    ssd1306_test_pattern();
    vTaskDelay(pdMS_TO_TICKS(1000));

    BaseType_t ok = xTaskCreatePinnedToCore(
        oled_task, "oled", 3072, NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "OLED task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OLED display task started (Core 0, 2 Hz)");
    return ESP_OK;
}

void oled_display_set_status(const char *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_status, status, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_message(const char *msg)
{
    if (!msg) return;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_message, msg, sizeof(s_message) - 1);
    s_message[sizeof(s_message) - 1] = '\0';
    s_message_set_at = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_provider(const char *info)
{
    if (!info) return;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_provider_info, info, sizeof(s_provider_info) - 1);
    s_provider_info[sizeof(s_provider_info) - 1] = '\0';
    strncpy(s_rotate_lines[0], info, 21);
    s_rotate_lines[0][21] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_tokens(uint32_t in_tokens, uint32_t out_tokens)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_token_info, sizeof(s_token_info), "In:%lu Out:%lu",
             (unsigned long)in_tokens, (unsigned long)out_tokens);
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_ip(const char *ip)
{
    if (!ip) return;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_ip_addr, ip, sizeof(s_ip_addr) - 1);
    s_ip_addr[sizeof(s_ip_addr) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_local_status(bool ollama_online, bool audio_online, bool apfel_online)
{
    portENTER_CRITICAL(&s_mux);
    s_ollama_online = ollama_online;
    s_apfel_online  = apfel_online;
    s_audio_online  = audio_online;
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_channel(const char *channel)
{
    if (!channel) return;

    /* Get day outside critical section (time/localtime may lock internally) */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int yday = tm_now.tm_yday;

    portENTER_CRITICAL(&s_mux);
    strncpy(s_channel, channel, sizeof(s_channel) - 1);
    s_channel[sizeof(s_channel) - 1] = '\0';
    if (yday != s_msg_count_yday) {
        s_msg_count_today = 0;
        s_msg_count_yday  = yday;
    }
    s_msg_count_today++;
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_rotate_line(int slot, const char *text)
{
    if (!text || slot < 0 || slot > 5) return;
    portENTER_CRITICAL(&s_mux);
    strncpy(s_rotate_lines[slot], text, 21);
    s_rotate_lines[slot][21] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_arm_header(const char *line)
{
    portENTER_CRITICAL(&s_mux);
    if (line) {
        strncpy(s_arm_hdr, line, 21);
        s_arm_hdr[21] = '\0';
    } else {
        s_arm_hdr[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_print_progress(int pct, int eta_mins, const char *filename)
{
    portENTER_CRITICAL(&s_mux);
    s_print_pct      = pct;
    s_print_eta_mins = eta_mins;
    if (filename && pct >= 0) {
        strncpy(s_print_fname, filename, sizeof(s_print_fname) - 1);
        s_print_fname[sizeof(s_print_fname) - 1] = '\0';
    } else {
        s_print_fname[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_alert(const char *line1, const char *line2, int duration_ms)
{
    portENTER_CRITICAL(&s_mux);
    if (line1) { strncpy(s_alert1, line1, sizeof(s_alert1) - 1); s_alert1[31] = '\0'; }
    if (line2) { strncpy(s_alert2, line2, sizeof(s_alert2) - 1); s_alert2[31] = '\0'; }
    s_alert_until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    portEXIT_CRITICAL(&s_mux);
}

void oled_display_set_ota(int pct, const char *state_label)
{
    portENTER_CRITICAL(&s_mux);
    s_ota_pct = pct;
    if (state_label) {
        strncpy(s_ota_label, state_label, sizeof(s_ota_label) - 1);
        s_ota_label[sizeof(s_ota_label) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}
