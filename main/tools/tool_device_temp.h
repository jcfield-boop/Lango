#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Read the ESP32-S3 internal chip temperature (tool interface).
 */
esp_err_t tool_device_temp_execute(const char *input_json, char *output, size_t output_size);

/**
 * Lightweight getter — returns chip temperature in Celsius.
 * Shares cached sensor handle with tool. Returns ESP_FAIL if sensor unavailable.
 */
esp_err_t device_temp_get_celsius(float *out_celsius);
