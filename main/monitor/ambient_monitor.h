#pragma once
#include <stddef.h>

/**
 * Ambient monitor — background tasks that feed live data to the OLED
 * without going through the LLM agent.
 *
 *  • Klipper poll (every 20s): print progress bar on OLED when printing
 *  • Sonos poll   (every 15s): now-playing in rotate slot 5
 *
 * ARM stock (rotate slot 4) is fed from heartbeat.c after each cycle
 * by calling ambient_parse_arm_stock().
 */

/**
 * Start the Klipper and Sonos background tasks.
 * Call once after services_config_load().
 */
void ambient_monitor_start(void);

/**
 * Parse the content of arm_stock_today.md into a short OLED line
 * (≤21 chars), e.g. "ARM $132.40 +2.1%".
 * Falls back to the first 21 chars of the file if no price pattern found.
 */
void ambient_parse_arm_stock(const char *content, char *out, size_t out_size);
