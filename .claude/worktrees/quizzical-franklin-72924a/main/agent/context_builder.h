#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Build the system prompt from bootstrap files (SOUL.md, USER.md)
 * and memory context (MEMORY.md + recent daily notes).
 *
 * @param buf   Output buffer (caller allocates, recommend LANG_CONTEXT_BUF_SIZE)
 * @param size  Buffer size
 */
esp_err_t context_build_system_prompt(char *buf, size_t size);

/**
 * Build a minimal system prompt for lightweight local models (e.g. Apfel).
 * Contains only core personality, user name/timezone, and current time.
 * No tools, no skills, no memory, no heartbeat. Targets ~400 tokens.
 */
esp_err_t context_build_minimal_prompt(char *buf, size_t size);

/**
 * Build the voice-router system prompt (EAGLE-style classifier).
 *
 * Reads /lfs/config/voice_router.md when present and falls back to the
 * compiled-in default when the file is missing. Prepends a tiny user-info
 * snippet so the router can personalize DIRECT answers ("Hi James, …").
 *
 * @param buf   Output buffer (recommend LANG_VOICE_ROUTER_PROMPT_MAX_BYTES)
 * @param size  Buffer size in bytes
 */
esp_err_t context_build_voice_router_prompt(char *buf, size_t size);

