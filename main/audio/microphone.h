#pragma once

#include "esp_err.h"

/**
 * Local microphone PTT (Push-to-Talk) driver.
 *
 * Monitors LANG_PTT_GPIO (GPIO 0, BOOT button, active low).
 * On press: opens a WAV audio ring, reads PCM from INMP441 via I2S RX.
 * On release (or 8s timeout): patches WAV sizes and commits to STT task.
 *
 * Requires i2s_audio_init() and audio_pipeline_init() to be called first.
 * Uses chat_id "ptt" — the agent response is delivered via local I2S speaker.
 */

/**
 * Configure GPIO 0 as PTT input (internal pull-up, active low).
 * Creates the PTT task pinned to Core 0.
 * Must be called after audio_pipeline_init() and i2s_audio_init().
 */
esp_err_t microphone_init(void);

/**
 * Start the PTT polling task.
 * Separate from microphone_init() so wake_word.c can use microphone_init()
 * for GPIO setup without starting the redundant polling task.
 */
esp_err_t microphone_start(void);
