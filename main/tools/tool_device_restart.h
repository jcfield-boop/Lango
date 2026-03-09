#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Tool: device_restart
 * Reboots the device cleanly after a short delay to allow the response to be sent.
 */
esp_err_t tool_device_restart_execute(const char *input_json, char *output, size_t output_size);
