#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize I2S TX channel for speaker output via MAX98357A.
 *
 * Creates an I2S standard-mode TX channel on I2S_NUM_0 at 16kHz, 16-bit mono.
 * GPIO assignments come from LANG_I2S_BCLK / LANG_I2S_LRCLK / LANG_I2S_DOUT
 * defined in langoustine_config.h.
 *
 * Must be called once during app_main, after LittleFS init.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2s_audio_init(void);

/**
 * @brief Play a WAV file synchronously through the I2S speaker.
 *
 * Parses the WAV header to extract sample rate and bit depth.
 * Reconfigures the I2S channel if parameters differ from the current config.
 * Writes the PCM payload in 4KB chunks via I2S DMA.
 * Returns only after all audio has been clocked out.
 *
 * Must be called from a task with sufficient stack (≥4KB recommended).
 * Not thread-safe — caller must ensure single-threaded access.
 *
 * @param wav_data  Pointer to WAV file data (RIFF header + PCM payload).
 * @param len       Total byte length of the WAV buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if header is malformed.
 */
esp_err_t i2s_audio_play_wav(const uint8_t *wav_data, size_t len);
