#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * UAC (USB Audio Class) microphone via the webcam's composite audio interface.
 *
 * Shares the USB host instance already installed by uvc_camera_init().
 * Must be called AFTER uvc_camera_init().
 *
 * PTT flow (BOOT button, GPIO0):
 *   Press   → LED_LISTENING, audio_ring_open_wav("ptt", ...)
 *   Held    → UAC PCM chunks drained into audio ring
 *   Release → audio_ring_patch_wav_sizes() + audio_ring_commit("ptt")
 *           → STT task wakes, Groq Whisper → agent
 */

esp_err_t uac_microphone_init(void);   /* install UAC driver, configure GPIO */
void      uac_microphone_start(void);  /* start PTT polling task on Core 0   */
void      uac_microphone_deinit(void);
bool      uac_microphone_available(void); /* true once UAC device is connected */
