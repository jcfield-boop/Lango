#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Audio pipeline: PSRAM ring buffer + Core-1 STT task coordination.
 *
 * Browser flow:
 *   1. Browser sends {"type":"audio_start"} → audio_ring_open()
 *   2. Browser sends binary WS frames    → audio_ring_append() (Core 0, httpd task)
 *   3. Browser sends {"type":"audio_end"} → audio_ring_commit() (signals Core 1 STT task)
 *
 * Local mic flow:
 *   1. PTT pressed / wake word → audio_ring_open_wav()  (writes 44-byte WAV header)
 *   2. i2s_audio_read() loop  → audio_ring_append()      (raw PCM)
 *   3. PTT released / VAD end → audio_ring_patch_wav_sizes() → audio_ring_commit()
 *
 *   4. STT task wakes, POSTs WAV/Opus to Groq Whisper, pushes transcript to message_bus
 *   5. agent_loop processes the transcript as a normal text prompt
 */

esp_err_t audio_pipeline_init(void);

/** Open a new recording session (resets ring buffer).
 *  @param channel  Message bus channel for STT result ("websocket", "telegram", etc.) */
esp_err_t audio_ring_open(const char *chat_id, const char *mime, const char *channel);

/**
 * Open a new recording session and write a standard 44-byte WAV header.
 * Sizes in the header are set to 0 and must be patched by audio_ring_patch_wav_sizes()
 * before calling audio_ring_commit().
 * @param channel  Message bus channel for STT result ("websocket", "telegram", etc.)
 */
esp_err_t audio_ring_open_wav(const char *chat_id, uint32_t sample_rate,
                               uint16_t channels, uint16_t bits,
                               const char *channel);

/** Append raw audio bytes to the ring buffer. Call from Core 0 (httpd) or mic task. */
esp_err_t audio_ring_append(const uint8_t *data, size_t len);

/**
 * Patch the WAV RIFF and data chunk sizes in the ring buffer based on the
 * current write position. Must be called after all PCM has been appended and
 * before audio_ring_commit(). Only valid after audio_ring_open_wav().
 */
esp_err_t audio_ring_patch_wav_sizes(void);

/** Signal that recording is complete; wakes the STT task on Core 1. */
esp_err_t audio_ring_commit(const char *chat_id);

/** Abort the current recording and reset the ring buffer. */
esp_err_t audio_ring_reset(void);

/** Abort only if the ring is owned by chat_id. Returns ESP_ERR_NOT_FOUND if mismatch. */
esp_err_t audio_ring_reset_for_client(const char *chat_id);
