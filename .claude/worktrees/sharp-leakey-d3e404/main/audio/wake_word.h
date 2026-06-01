#pragma once

#include "esp_err.h"
#include <stdbool.h>

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

/** Suspend wake word tasks (releases I2S RX for mic_test / mic_playback). */
void wake_word_suspend(void);

/** Resume wake word tasks after suspend. */
void wake_word_resume(void);

/** True if wake word tasks are created (may be suspended). */
bool wake_word_is_running(void);

/**
 * Set the software pre-gain applied to mic samples before AFE feed.
 * Range: 0.1 – 50.0 (default: 3.0). Applied as fixed-point multiply in feed loop.
 * Saved to NVS ("ww_config"/"gain") for persistence across reboots.
 * Takes effect immediately (no restart needed).
 */
void wake_word_set_gain(float gain);

/** Get the current software pre-gain value. */
float wake_word_get_gain(void);

/**
 * Set the WakeNet detection threshold.
 * Range: 0.0 – 0.9999 (default: 0.5). Lower = more sensitive, more false positives.
 * Saved to NVS ("ww_config"/"threshold") for persistence across reboots.
 * Takes effect immediately on the running AFE.
 */
void wake_word_set_threshold(float threshold);

/** Get the current WakeNet threshold value. */
float wake_word_get_threshold(void);

/* ── Commissioning / test mode ──────────────────────────────── */

/** Snapshot of test counters for CLI readout. */
typedef struct {
    uint32_t feeds;
    uint32_t fetches;
    uint32_t speech;     /* VAD speech frames */
    uint32_t wakeups;    /* WAKENET_DETECTED count */
    float    volume_db;  /* last AFE data_volume */
    int16_t  peak_pos;   /* max sample since test start */
    int16_t  peak_neg;   /* min sample since test start */
    float    gain;       /* current software gain */
    float    threshold;  /* current WakeNet threshold */
} wake_word_test_result_t;

/**
 * Start test mode: resets counters and enables per-event logging for N seconds.
 * Say "Hi ESP" during this window — every VAD speech frame and wakeup event
 * is printed to the serial console.
 */
void wake_word_test_start(int seconds);

/** Stop test mode early. */
void wake_word_test_stop(void);

/** Read current test counters (non-destructive snapshot). */
void wake_word_test_snapshot(wake_word_test_result_t *out);
