#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Audio pipeline: PSRAM ring buffer + Core-1 STT task coordination.
 *
 * Flow:
 *   1. Browser sends {"type":"audio_start"} → audio_ring_open()
 *   2. Browser sends binary WS frames    → audio_ring_append() (Core 0, httpd task)
 *   3. Browser sends {"type":"audio_end"} → audio_ring_commit() (signals Core 1 STT task)
 *   4. STT task wakes, calls stt_transcribe(), pushes result to message_bus as inbound prompt
 *   5. agent_loop processes the transcript as a normal text prompt
 */

esp_err_t audio_pipeline_init(void);

/** Open a new recording session (resets ring buffer). */
esp_err_t audio_ring_open(const char *chat_id, const char *mime);

/** Append raw audio bytes to the ring buffer. Call from Core 0 (httpd). */
esp_err_t audio_ring_append(const uint8_t *data, size_t len);

/** Signal that recording is complete; wakes the STT task on Core 1. */
esp_err_t audio_ring_commit(const char *chat_id);

/** Abort the current recording and reset the ring buffer. */
esp_err_t audio_ring_reset(void);
