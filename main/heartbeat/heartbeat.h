#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the heartbeat service (logs ready state).
 */
esp_err_t heartbeat_init(void);

/**
 * Start the heartbeat timer. Checks HEARTBEAT.md periodically
 * and sends a prompt to the agent if actionable tasks are found.
 */
esp_err_t heartbeat_start(void);

/**
 * Stop and delete the heartbeat timer.
 */
void heartbeat_stop(void);

/**
 * Manually trigger a heartbeat check (for CLI testing).
 * Returns true if the agent was prompted, false if no tasks found.
 */
bool heartbeat_trigger(void);

/**
 * Get today's executed heartbeat tasks as a text log.
 * Writes lines like "- 06:05 Morning briefing\n" into buf.
 * Returns number of entries (0 if none today). Resets on new day.
 */
int heartbeat_get_today_log(char *buf, size_t size);

/**
 * Get the next upcoming daily task (e.g. "06:00 Briefing").
 * Writes into buf. Returns true if found, false if none pending.
 */
bool heartbeat_get_next_task(char *buf, size_t size);
