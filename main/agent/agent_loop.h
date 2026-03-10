#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the agent loop.
 */
esp_err_t agent_loop_init(void);

/**
 * Start the agent loop task (runs on Core 1).
 * Consumes from inbound queue, calls Claude API, pushes to outbound queue.
 */
esp_err_t agent_loop_start(void);

/**
 * Returns true while the agent is processing a turn (between message_bus_pop and free(msg.content)).
 * Safe to call from any task — uses atomic load.
 */
bool agent_loop_is_busy(void);
