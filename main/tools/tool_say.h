#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Speak text aloud via TTS → I2S speaker, bypassing the LLM.
 *
 * Calls Groq PlayAI TTS directly and plays the resulting WAV
 * through the MAX98357A speaker. No LLM round-trip required.
 *
 * Input JSON fields:
 *   text  (string, required) — text to speak aloud
 *
 * Output: "OK: spoken N bytes" or error description.
 */
esp_err_t tool_say_execute(const char *input_json, char *output, size_t output_size);

/**
 * Speak text directly (C API, no JSON parsing).
 * Convenience for boot greeting and other internal callers.
 * Returns ESP_OK on success.
 */
esp_err_t say_speak(const char *text);
