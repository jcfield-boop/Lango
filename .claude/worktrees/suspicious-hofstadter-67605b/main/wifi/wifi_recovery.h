#pragma once

/* ── wifi_recovery — auto-reconnect on cascading outbound failures ──
 *
 * Background: device's wifi association can degrade silently. Symptoms:
 *   - DNS lookups return EAI_FAIL (errno 202)
 *   - TCP connect() hangs in EALREADY for 30+ seconds
 *   - TLS handshake gets RST mid-flight
 * RSSI looks healthy throughout. The only reliable recovery is to
 * disassociate + reassociate (esp_wifi_disconnect + esp_wifi_connect),
 * which kicks DHCP, refreshes DNS resolvers, and clears stuck sockets.
 *
 * This module tracks consecutive outbound HTTP failures across all
 * subsystems (telegram, LLM, STT, TTS, preflights) and triggers
 * a wifi reconnect when a threshold is crossed. Successes reset the
 * counter — a single good call clears the tally.
 *
 * Thread-safe. The reconnect itself runs on a dedicated worker so
 * caller tasks never block.
 */

#include "esp_err.h"
#include <stdbool.h>

/** Initialise. Spawns the recovery worker task. Idempotent. */
esp_err_t wifi_recovery_init(void);

/** Record an outbound HTTP failure. Caller is identified by `who`
 *  (short string used for logs, e.g. "telegram", "llm", "stt"). */
void wifi_recovery_record_failure(const char *who);

/** Record an outbound HTTP success. Resets the consecutive-failure
 *  counter. Cheap to call on every successful request. */
void wifi_recovery_record_success(const char *who);

/** True if the worker is currently mid-reconnect — callers can use
 *  this to skip subsequent failures while recovery is in flight. */
bool wifi_recovery_in_progress(void);

/** Get current consecutive-failure count (for diagnostics / OLED). */
int wifi_recovery_consecutive_failures(void);
