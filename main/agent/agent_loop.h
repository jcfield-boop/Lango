#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the agent loop.
 */
esp_err_t agent_loop_init(void);

/**
 * Start the agent loop task (runs on Core 1).
 * Consumes from inbound queue, calls Claude API, pushes to outbound queue.
 */
esp_err_t agent_loop_start(void);

/**
 * Returns true while the agent is processing a turn (between message_bus_pop and free(msg.content)).
 * Safe to call from any task — uses atomic load.
 */
bool agent_loop_is_busy(void);

/* ── Diagnostic state for external watchdog ─────────────────────── */

/** How long (ms) the current turn has been running. Returns 0 if idle. */
int64_t agent_loop_turn_age_ms(void);

/** Copy the current phase label into buf (e.g. "llm_cloud", "tool:weather"). */
void agent_loop_get_phase(char *buf, size_t len);

/** Set LLM API rate limit (max requests per hour). */
void agent_set_rate_limit(int max_per_hour);

/** Get current rate limit setting (requests per hour). */
int agent_get_rate_limit(void);

/** Get number of LLM requests in current rate window. */
int agent_get_rate_count(void);

/* ── Voice router kill switch (Slice 2) ──────────────────────────
 * Backed by NVS key LANG_NVS_KEY_VOICE_ROUTER under LANG_NVS_LLM.
 * Default false — flip to true via CLI (voice_router on) once the
 * router is verified in Slice 3. Read on every voice turn from the
 * agent loop; cheap (in-RAM flag, NVS only touched on set). */
bool agent_voice_router_enabled(void);
void agent_set_voice_router_enabled(bool enabled);
