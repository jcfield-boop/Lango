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

/** Set local TTS base URL (e.g. "http://192.168.0.25:8000").
 *  When set, tts_generate() tries local first, falls back to cloud. */
void tts_set_local_url(const char *url);

/** Get local TTS base URL (empty string if not set). */
const char *tts_get_local_url(void);

/** Set local TTS model name (e.g. "mlx-community/Kokoro-82M-bf16").
 *  Defaults to Kokoro-82M-bf16 if not set. */
void tts_set_local_model(const char *model);

/** Set local TTS voice name (e.g. "af_heart", "af_sky", "am_adam").
 *  Defaults to "af_heart" if not set. */
void tts_set_local_voice(const char *voice);

/** Check if local TTS is configured and not in backoff. */
bool tts_local_is_online(void);
