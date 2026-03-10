#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OTA_STATE_IDLE        = 0,
    OTA_STATE_PENDING     = 1,   /* waiting for agent to finish */
    OTA_STATE_DOWNLOADING = 2,
    OTA_STATE_VERIFYING   = 3,
    OTA_STATE_REBOOTING   = 4,
    OTA_STATE_ERROR       = 5,
} ota_state_t;

typedef struct {
    ota_state_t state;
    uint8_t     progress_pct;
    char        new_version[32];
    char        error_msg[64];
} ota_status_t;

/**
 * Get a snapshot of the current OTA status (thread-safe).
 */
ota_status_t ota_get_status(void);

/**
 * Perform OTA firmware update from a URL (blocking).
 * Downloads the firmware binary and applies it. Reboots on success.
 * Waits up to 30s for the agent to finish its current turn before starting.
 * Safe to call from a serial CLI handler.
 *
 * @param url  HTTPS URL to the firmware .bin file
 * @return ESP_OK on success (device will reboot), error code otherwise
 */
esp_err_t ota_update_from_url(const char *url);

/**
 * Start an OTA update in a background task (non-blocking).
 * Use from web/Telegram handlers that must return promptly.
 * Progress is broadcast to the live log monitor channel.
 *
 * @param url  HTTPS URL to the firmware .bin file
 * @return ESP_OK if the task was started, ESP_ERR_INVALID_STATE if one is already running,
 *         ESP_ERR_NO_MEM if there is insufficient heap to start the task
 */
esp_err_t ota_start_async(const char *url);
