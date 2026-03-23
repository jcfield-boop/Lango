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

static void draw_idle_screen(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int rssi = get_rssi();

    /* ── Row 0-15: Large time (HH:MM) on left, IP on right ──────── */
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    ssd1306_str_2x(0, 0, time_str);
    /* 5 chars × 12px = 60px for 2x time. IP starts after. */

    portENTER_CRITICAL(&s_mux);
    char ip_copy[20];
    strncpy(ip_copy, s_ip_addr, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (ip_copy[0]) {
        /* Small 1x font IP right-aligned on row 0 (above the 2x baseline) */
        int ip_len = (int)strlen(ip_copy);
        int ip_x = 128 - ip_len * 6;
        if (ip_x < 64) ip_x = 64;  /* don't overlap time */
        ssd1306_str(ip_x, 0, ip_copy);
    }

    /* ── Row 18: date (left) + RSSI bars (right) ─────────────────── */
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    char date_str[22];
    snprintf(date_str, sizeof(date_str), "%s %s %d",
             days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday);
    ssd1306_str(0, 18, date_str);

    /* RSSI bars right-aligned on the date row */
    draw_rssi(106, 18, rssi);

    /* Separator line */
    ssd1306_hline(0, 28, 128, true);

    /* ── Row 30: provider/model info ────────────────────────────── */
    portENTER_CRITICAL(&s_mux);
    char prov_copy[32];
    strncpy(prov_copy, s_provider_info, sizeof(prov_copy) - 1);
    prov_copy[sizeof(prov_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
    if (prov_copy[0]) {
        ssd1306_str(0, 30, prov_copy);
    } else {
        ssd1306_str(0, 30, "Ready");
    }

    /* ── Row 40-48: message preview or token counts ─────────────── */
    portENTER_CRITICAL(&s_mux);
    char msg_copy[128];
    strncpy(msg_copy, s_message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';
    char tok_copy[32];
    strncpy(tok_copy, s_token_info, sizeof(tok_copy) - 1);
    tok_copy[sizeof(tok_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (msg_copy[0]) {
        char line[22];
        strncpy(line, msg_copy, 21);
        line[21] = '\0';
        ssd1306_str(0, 40, line);

        if (strlen(msg_copy) > 21) {
            strncpy(line, msg_copy + 21, 21);
            line[21] = '\0';
            ssd1306_str(0, 48, line);
        }
    } else {
        if (tok_copy[0]) {
            ssd1306_str(0, 40, tok_copy);
        }

        /* System stats */
        uint32_t sram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char stat_line[32];
        snprintf(stat_line, sizeof(stat_line), "S:%luK P:%luM",
                 (unsigned long)(sram / 1024), (unsigned long)(psram / (1024*1024)));
        ssd1306_str(0, tok_copy[0] ? 48 : 40, stat_line);
    }

    /* ── Row 56: uptime ─────────────────────────────────────────── */
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    int hrs = (int)(uptime_s / 3600);
    int mins = (int)((uptime_s % 3600) / 60);
    char up_line[22];
    snprintf(up_line, sizeof(up_line), "Up:%dh%02dm", hrs, mins);
    ssd1306_str(0, 56, up_line);
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

    /* Row 12-20: Status (e.g. "Thinking...", "Tool: web_search") */
    ssd1306_str(0, 12, status_copy);

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

        /* Check if alert is active */
        bool alert_active = false;
        portENTER_CRITICAL(&s_mux);
        if (s_alert_until > 0 && xTaskGetTickCount() < s_alert_until) {
            alert_active = true;
        } else {
            s_alert_until = 0;
        }
        portEXIT_CRITICAL(&s_mux);

        if (alert_active) {
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

void oled_display_alert(const char *line1, const char *line2, int duration_ms)
{
    portENTER_CRITICAL(&s_mux);
    if (line1) { strncpy(s_alert1, line1, sizeof(s_alert1) - 1); s_alert1[31] = '\0'; }
    if (line2) { strncpy(s_alert2, line2, sizeof(s_alert2) - 1); s_alert2[31] = '\0'; }
    s_alert_until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    portEXIT_CRITICAL(&s_mux);
}
