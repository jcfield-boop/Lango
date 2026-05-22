#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Generate an AI image from a prompt and display it on the Samsung Frame TV.
 *
 * Sends the prompt to the Nanoframe Mac app (default: http://192.168.0.51:11436/generate).
 * The app calls DALL-E 3, upscales to 4K, and pushes to the TV via local network.
 * Responds 202 immediately — generation happens asynchronously (~30-90s).
 *
 * Input JSON fields:
 *   prompt  (string, required)  — description of the image to generate
 *
 * Set LANG_NANOFRAME_URL in langoustine_config.h or via `set_config nanoframe_url <url>`.
 */
esp_err_t tool_frame_tv_execute(const char *input_json, char *output, size_t output_size);
