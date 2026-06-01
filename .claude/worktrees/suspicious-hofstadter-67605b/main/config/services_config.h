#pragma once
#include "esp_err.h"

/**
 * Parse /lfs/config/SERVICES.md and populate module API keys
 * that were not already set via NVS.
 *
 * Priority: NVS > SERVICES.md > build-time defaults.
 *
 * Call AFTER all module inits (llm_proxy_init, stt_client_init, etc.)
 * so NVS values are already loaded, but BEFORE agent_loop_start().
 */
esp_err_t services_config_load(void);    /* startup: apply only if NVS key not set */
esp_err_t services_config_reload(void);  /* runtime: force-apply all values (overrides NVS) */

/** Get the webhook HMAC secret from SERVICES.md. Returns NULL if not configured. */
const char *services_get_webhook_secret(void);
