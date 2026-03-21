#include "display/oled_display.h"
#include "display/ssd1306.h"
#include "langoustine_config.h"
#include "led/led_indicator.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

/* ── Thread-safe state (written by any task, read by display task) ── */

static char s_status[32]  = "Booting";
static char s_message[128] = "";
static char s_alert1[32]  = "";
static char s_alert2[32]  = "";
static TickType_t s_alert_until = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── State label from LED state ──────────────────────────────────── */

static const char *state_label(led_state_t st)
{
    switch (st) {
        case LED_BOOTING:    return "Booting...";
        case LED_WIFI:       return "WiFi...";
        case LED_READY:      return "";   /* idle — show clock instead */
        case LED_THINKING:   return "Thinking...";
        case LED_SPEAKING:   return "Speaking...";
        case LED_LISTENING:  return "Listening...";
        case LED_ERROR:      return "Error";
        case LED_OTA:        return "OTA Update...";
        case LED_CAPTURING:  return "Capturing...";
        case LED_FLASH_FADE: return "Processing...";
        default:             return "";
    }
}

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
    /* Row 0-15: Large time (HH:MM) */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    ssd1306_str_2x(0, 0, time_str);

    /* RSSI bars in top-right */
    draw_rssi(106, 0, get_rssi());

    /* Row 18: Date line */
    char date_str[22];
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(date_str, sizeof(date_str), "%s %s %d",
             days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday);
    ssd1306_str(0, 18, date_str);

    /* Row 18 right: seconds indicator (dot blinks) */
    if (tm.tm_sec % 2 == 0) {
        ssd1306_fill_rect(100, 20, 2, 2, true);
    }

    /* Separator line */
    ssd1306_hline(0, 28, 128, true);

    /* Row 30-37: status or "Ready" */
    portENTER_CRITICAL(&s_mux);
    char status_copy[32];
    strncpy(status_copy, s_status, sizeof(status_copy) - 1);
    status_copy[sizeof(status_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (status_copy[0]) {
        ssd1306_str(0, 30, status_copy);
    } else {
        ssd1306_str(0, 30, "Ready");
    }

    /* Row 40-55: message preview (up to 2 lines of 21 chars) */
    portENTER_CRITICAL(&s_mux);
    char msg_copy[128];
    strncpy(msg_copy, s_message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    if (msg_copy[0]) {
        /* Line 1: first 21 chars */
        char line[22];
        strncpy(line, msg_copy, 21);
        line[21] = '\0';
        ssd1306_str(0, 40, line);

        /* Line 2: next 21 chars if available */
        if (strlen(msg_copy) > 21) {
            strncpy(line, msg_copy + 21, 21);
            line[21] = '\0';
            ssd1306_str(0, 48, line);
        }

        /* Line 3: overflow indicator */
        if (strlen(msg_copy) > 42) {
            ssd1306_str(0, 56, "...");
        }
    }
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
            draw_idle_screen();
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
