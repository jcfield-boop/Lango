#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * TTS client — Groq PlayAI-compatible JSON POST, WAV response.
 * Audio is buffered in PSRAM, served via /tts/<id>.
 *
 * Endpoint:  NVS "tts_config" key "endpoint"  (default: Groq TTS)
 * Model:     NVS "tts_config" key "model"
 * Voice:     NVS "tts_config" key "voice"
 * API key:   NVS "tts_config" key "api_key"
 */

/** Load NVS config. Call once at startup. */
esp_err_t tts_client_init(void);

/**
 * Generate TTS audio for text.
 * On success, writes an 8-char hex ID into id_out (must be >= 9 bytes).
 * Audio is cached in PSRAM and served at /tts/<id>.
 */
esp_err_t tts_generate(const char *text, char *id_out);

/**
 * Look up a cached audio buffer by 8-char hex ID.
 * Sets *buf_out and *len_out if found and not expired (5-min TTL).
 * Returns ESP_OK on hit, ESP_ERR_NOT_FOUND otherwise.
 * The buffer is in PSRAM — do NOT free it.
 */
esp_err_t tts_cache_get(const char *id, const uint8_t **buf_out, size_t *len_out);

/** Save API key to NVS and update in-memory key. */
void tts_set_api_key(const char *key);

/** Save voice to NVS and update in-memory voice. */
void tts_set_voice(const char *voice);

/** Save model to NVS and update in-memory model. */
void tts_set_model(const char *model);

/** Copy masked API key into buf. */
void tts_get_api_key_masked(char *buf, size_t buf_len);

/** Copy current voice into buf. */
void tts_get_voice(char *buf, size_t buf_len);
