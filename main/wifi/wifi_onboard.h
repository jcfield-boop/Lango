#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * WiFi onboarding captive portal.
 *
 * When no WiFi credentials are stored (NVS empty, no build-time secrets),
 * starts a SoftAP "Langoustine-XXXX" with a captive portal web page.
 * User connects from phone/laptop, enters WiFi SSID/password + API key,
 * device saves to NVS and reboots into STA mode.
 */

/** Check if WiFi credentials exist in NVS or build-time config. */
bool wifi_onboard_has_credentials(void);

/**
 * Start the onboarding captive portal.
 * Blocks until user submits credentials, then reboots.
 * Call this instead of wifi_manager_start() when has_credentials() returns false.
 */
void wifi_onboard_start(void);
