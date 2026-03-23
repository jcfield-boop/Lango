#pragma once

/**
 * OLED Display Manager — ambient status dashboard on SSD1306
 *
 * Provides a persistent status display with:
 *   - Idle: time (large), date, WiFi RSSI, next cron job
 *   - Active: current state (Listening/Thinking/Speaking) + last response
 *   - Alert: OTA progress, errors
 *
 * Thread-safe: any task can call oled_display_set_* functions.
 */

#include "esp_err.h"
#include "driver/i2c_master.h"

/** Initialise the OLED display on an existing I2C bus and start the
 *  refresh task (Core 0, priority 2, 3KB stack). */
esp_err_t oled_display_init(i2c_master_bus_handle_t bus);

/** Set the status line (e.g. "Listening...", "Thinking...", "Ready").
 *  Thread-safe. Automatically shown on the active screen. */
void oled_display_set_status(const char *status);

/** Set a message preview (truncated last agent response).
 *  Thread-safe. Shown below the status on the active screen. */
void oled_display_set_message(const char *msg);

/** Set the LLM provider/model info line (e.g. "qwen2.5:14b (local)").
 *  Thread-safe. Shown on both idle and active screens. */
void oled_display_set_provider(const char *info);

/** Update session token counts. Thread-safe. */
void oled_display_set_tokens(uint32_t in_tokens, uint32_t out_tokens);

/** Set the device IP address for display. Thread-safe. */
void oled_display_set_ip(const char *ip);

/** Temporarily show an alert for `duration_ms` (e.g. cron fire, OTA).
 *  After the duration, reverts to normal display. Thread-safe. */
void oled_display_alert(const char *line1, const char *line2, int duration_ms);
