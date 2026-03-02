#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * STT client — Groq Whisper-compatible multipart/form-data upload.
 *
 * Endpoint:  NVS "stt_config" key "endpoint"  (default: Groq Whisper)
 * Model:     NVS "stt_config" key "model"
 * API key:   NVS "stt_config" key "api_key"
 */

typedef struct {
    char text[1024];   /* transcribed text (null-terminated) */
    char error[256];   /* error description if transcription failed */
} stt_result_t;

/** Load NVS config. Call once at startup. */
esp_err_t stt_client_init(void);

/** POST audio to STT endpoint, return transcribed text. */
esp_err_t stt_transcribe(const uint8_t *audio, size_t len,
                          const char *mime, stt_result_t *out);

/** Save API key to NVS and update in-memory key. */
void stt_set_api_key(const char *key);

/** Copy masked API key into buf (first 4 chars + "****"). */
void stt_get_api_key_masked(char *buf, size_t buf_len);
