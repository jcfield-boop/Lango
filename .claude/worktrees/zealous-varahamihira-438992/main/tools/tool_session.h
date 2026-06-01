#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Tool: session_clear
 * Deletes the conversation history file for a given chat_id.
 * Schema: { "chat_id": string }
 */
esp_err_t tool_session_clear_execute(const char *input_json, char *output, size_t output_size);
