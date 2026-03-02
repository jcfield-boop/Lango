#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute capture_image tool.
 *
 * Captures a JPEG frame from the connected USB webcam, saves it to
 * LANG_CAMERA_CAPTURE_PATH, then sends it to the Claude vision API and
 * returns the description. Falls back to reporting the saved path if no
 * API key is configured.
 *
 * Input JSON (all optional):
 *   { "prompt": "What do you see?" }
 */
esp_err_t tool_capture_image_execute(const char *input_json,
                                     char *output, size_t output_size);
