#pragma once

#include "esp_err.h"

/**
 * Wake-word detection and local voice activation via ESP-SR.
 *
 * Uses the ESP-SR AFE (Audio Front End) with WakeNet9, VAD, and noise
 * suppression. Continuously feeds audio from the INMP441 (I2S RX) through
 * the AFE pipeline.
 *
 * Activation triggers (either starts a recording session):
 *   - "Hi ESP" detected by WakeNet9
 *   - LANG_PTT_GPIO (GPIO 0) held low
 *
 * End of recording:
 *   - VAD silence detected after speech
 *   - PTT button released
 *   - 10 s timeout without VAD speech
 *
 * On activation: LED → LED_LISTENING; audio ring opened as WAV.
 * On commit: audio_ring_patch_wav_sizes() + audio_ring_commit() → STT task.
 *
 * Requires microphone_init() (for GPIO config), i2s_audio_init(), and
 * audio_pipeline_init() to be called before wake_word_start().
 */

/**
 * Initialize the ESP-SR AFE.
 * Returns ESP_ERR_NOT_SUPPORTED if the esp-sr component is unavailable.
 */
esp_err_t wake_word_init(void);

/**
 * Start the wake-word detection task (pinned to Core 0).
 * Returns ESP_ERR_NOT_SUPPORTED if esp-sr initialization failed.
 * On failure, caller should fall back to microphone_start() for PTT-only.
 */
esp_err_t wake_word_start(void);
