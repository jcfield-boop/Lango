#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Send a push notification via ntfy.sh (or a self-hosted ntfy server).
 *
 * Input JSON fields:
 *   message   (string, required)  — notification body
 *   title     (string, optional)  — notification title
 *   priority  (string, optional)  — min|low|default|high|urgent
 *   tags      (string, optional)  — comma-separated ntfy tags/emojis
 *   topic     (string, optional)  — ntfy topic (overrides NVS default)
 *
 * NVS: notify_config ns, keys "topic" and "server".
 * Default server: https://ntfy.sh
 *
 * Output: "OK" on success or error description.
 */
esp_err_t tool_notify_execute(const char *input_json, char *output, size_t output_size);
