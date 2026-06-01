#pragma once

#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Set speaker volume.
 * Input JSON: {"level": <0-100>}  (percentage; 100 = full, 50 = half, 0 = mute)
 */
esp_err_t tool_set_volume_execute(const char *input_json, char *output, size_t output_size);
