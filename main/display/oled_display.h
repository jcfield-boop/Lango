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

/** Set local service status indicators (Ollama + mlx-audio + Apfel). Thread-safe. */
void oled_display_set_local_status(bool ollama_online, bool audio_online, bool apfel_online);

/** Set the active channel ("WS", "TG", "PTT", "SYS"). Thread-safe.
 *  Also increments a daily message counter (resets at midnight). */
void oled_display_set_channel(const char *channel);

/** Set a rotating info line (slot 0-5, 5s each). Thread-safe.
 *  Slot 0: provider (auto-set by set_provider).
 *  Slot 1: next heartbeat task / ctx-bloat warning.
 *  Slot 2: voice router decision.
 *  Slot 3: rate limit.
 *  Slot 4: ARM stock price (set by heartbeat after armpre01).
 *  Slot 5: Sonos now-playing (set by ambient_monitor). */
void oled_display_set_rotate_line(int slot, const char *text);

/** Set the ARM stock line shown persistently in the top-right header.
 *  Format e.g. "ARM $309 +2.3%". Pass empty string to clear. Thread-safe. */
void oled_display_set_arm_header(const char *line);

/** Show a 3D print progress bar in the stats row while printing.
 *  pct: 0-100 (pass -1 to clear and show normal SRAM stats).
 *  eta_mins: estimated minutes remaining (-1 if unknown).
 *  filename: current gcode filename (may be NULL). Thread-safe. */
void oled_display_set_print_progress(int pct, int eta_mins, const char *filename);

/** Temporarily show an alert for `duration_ms` (e.g. cron fire, OTA).
 *  After the duration, reverts to normal display. Thread-safe. */
void oled_display_alert(const char *line1, const char *line2, int duration_ms);

/** Set OTA progress for dedicated OTA screen. pct=0-100, state label shown above bar.
 *  Pass pct<0 to dismiss the OTA screen. Thread-safe. */
void oled_display_set_ota(int pct, const char *state_label);
