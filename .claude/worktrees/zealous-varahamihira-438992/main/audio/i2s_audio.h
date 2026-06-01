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

/**
 * @brief Read raw 16kHz 16-bit mono PCM from the INMP441 microphone.
 *
 * Reads up to buf_size bytes from the I2S RX channel (always 16kHz, 16-bit mono).
 * Thread-safe to call from any task, but only one task should call this at a time.
 *
 * @param buf         Buffer to receive PCM samples.
 * @param buf_size    Size of buf in bytes.
 * @param bytes_read  Output: actual bytes read.
 * @param timeout_ms  Timeout in milliseconds.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if RX not ready.
 */
esp_err_t i2s_audio_read(uint8_t *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Play a WAV file asynchronously through the I2S speaker.
 *
 * Starts playback in a background task and returns immediately.
 * If already playing, the current playback is cancelled and the new WAV starts.
 * The WAV data must remain valid (in PSRAM cache) for the duration of playback.
 *
 * @param wav_data  Pointer to WAV file data (RIFF header + PCM payload).
 * @param len       Total byte length of the WAV buffer.
 * @return ESP_OK on success.
 */
esp_err_t i2s_audio_play_wav_async(const uint8_t *wav_data, size_t len);

/**
 * @brief Enqueue a WAV to play back-to-back after the currently playing item.
 *
 * Unlike i2s_audio_play_wav_async() (which cancels the current item and
 * replaces the queue), this appends to the playback queue with no gap.
 * Used by the voice pipeline to stream successive TTS segments: the first
 * sentence plays via play_wav_async while the second sentence's TTS is
 * still generating, then the second WAV is appended via this function.
 *
 * The WAV data must remain valid (in PSRAM cache) until playback completes.
 *
 * @param wav_data  Pointer to WAV file data (RIFF header + PCM payload).
 * @param len       Total byte length of the WAV buffer.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the queue is full.
 */
esp_err_t i2s_audio_enqueue_wav(const uint8_t *wav_data, size_t len);

/**
 * @brief Stop any in-progress async playback.
 */
void i2s_audio_stop(void);

/**
 * @brief Set playback volume (0=mute, 128=50%, 255=full). Persisted to NVS.
 */
void i2s_audio_set_volume(uint8_t vol);

/**
 * @brief Get current playback volume (0–255).
 */
uint8_t i2s_audio_get_volume(void);

/**
 * @brief Restart the I2S RX channel (disable + enable).
 *
 * Call after suspending the wake word feed task to clear any pending
 * i2s_channel_read that was holding the DMA semaphore.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2s_audio_rx_restart(void);

/**
 * @brief Play a test tone (440 Hz sine wave, ~2 seconds) through the speaker.
 *
 * Generates PCM samples in-memory — no TTS API call needed.
 * Useful for verifying the I2S → amp → speaker hardware path.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2s_audio_test_tone(void);

/**
 * @brief Run I2S RX diagnostics — register dump, raw DMA read, wiring checklist.
 *
 * Prints results directly to stdout (serial console).
 * Suspends wake word before calling if needed.
 */
void i2s_audio_diag(void);
