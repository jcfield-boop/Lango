#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize the USB host stack and UVC driver.
 * Does NOT start the USB host event task — call uvc_camera_start_host_task()
 * AFTER all USB class drivers (UVC, UAC, etc.) have been installed so every
 * driver is registered before the first device-connected event fires.
 * Safe to call even if no camera is connected.
 */
esp_err_t uvc_camera_init(void);

/**
 * Start the USB host library event task.
 * Must be called after uvc_camera_init() AND after any other USB class
 * driver installs (e.g. uac_microphone_init()) to avoid a registration race
 * where a late-registering driver misses the device-connected callback.
 */
void uvc_camera_start_host_task(void);

/**
 * Deinitialize the UVC driver and USB host stack.
 */
void uvc_camera_deinit(void);

/**
 * Returns true if a UVC camera device is currently detected on the USB bus.
 */
bool uvc_camera_is_connected(void);

/**
 * Capture a single JPEG frame from the connected USB webcam.
 *
 * Targets 640×480 MJPEG @ 15fps. Falls back to 320×240 if the camera
 * doesn't support 640×480. Blocks until a frame arrives or timeout elapses.
 *
 * @param buf          Caller-allocated PSRAM buffer (see LANG_CAMERA_BUF_SIZE)
 * @param buf_size     Size of buf in bytes
 * @param jpeg_len     OUT: actual JPEG byte count written to buf
 * @param timeout_ms   Max time to wait for a frame
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no device,
 *         ESP_ERR_TIMEOUT if no frame arrived in time, ESP_FAIL on other errors
 */
esp_err_t uvc_camera_capture(uint8_t *buf, size_t buf_size,
                              size_t *jpeg_len, int timeout_ms);
