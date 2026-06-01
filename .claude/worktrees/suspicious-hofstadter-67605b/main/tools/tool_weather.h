#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Get current weather and 3-day forecast from wttr.in (no API key required).
 *
 * Input JSON fields:
 *   location  (string, optional)  — city, zip, or "lat,lon". If omitted,
 *                                    uses NVS default (weather_config/location)
 *                                    or wttr.in IP-based auto-detection.
 *
 * Output: plain-text summary with current conditions and 3-day forecast.
 */
esp_err_t tool_weather_execute(const char *input_json, char *output, size_t output_size);
