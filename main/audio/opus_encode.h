#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Encode 16-bit PCM audio to Opus in OGG container.
 *
 * Takes raw 16-bit mono PCM samples and produces a valid OGG/Opus stream
 * that Groq Whisper (and other STT APIs) can accept as "audio/ogg".
 *
 * @param pcm_data    Input: 16-bit signed PCM samples (mono)
 * @param pcm_bytes   Input: size of PCM data in bytes
 * @param sample_rate Input sample rate (typically 16000)
 * @param out_data    Output: allocated OGG/Opus buffer (caller must free with free())
 * @param out_size    Output: size of encoded data in bytes
 * @return ESP_OK on success
 */
esp_err_t opus_encode_pcm_to_ogg(const int16_t *pcm_data, size_t pcm_bytes,
                                  uint32_t sample_rate,
                                  uint8_t **out_data, size_t *out_size);
