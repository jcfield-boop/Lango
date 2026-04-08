#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "langoustine_config.h"

/**
 * Initialize the LLM proxy. Reads API key and model from build-time secrets, then NVS.
 */
esp_err_t llm_proxy_init(void);

/**
 * Save the LLM API key to NVS.
 */
esp_err_t llm_set_api_key(const char *api_key);

/**
 * Save the LLM provider to NVS. (e.g. "anthropic", "openai")
 */
esp_err_t llm_set_provider(const char *provider);

/**
 * Save the model identifier to NVS.
 */
esp_err_t llm_set_model(const char *model);

/**
 * Return current provider string (e.g. "openrouter", "anthropic").
 */
const char *llm_get_provider(void);

/**
 * Return current model string.
 */
const char *llm_get_model(void);

/**
 * Return current API key (raw, for masking by caller).
 */
const char *llm_get_api_key(void);

/**
 * Set the base URL for local/Ollama models (e.g. "http://192.168.0.25:11434/v1").
 * Only used when provider is "ollama".
 */
void llm_set_local_url(const char *url);

/**
 * Return current local model base URL (empty string if not set).
 */
const char *llm_get_local_url(void);

/**
 * Return cumulative session token stats and estimated cost.
 * @param in             Total input tokens this session (may be NULL)
 * @param out            Total output tokens this session (may be NULL)
 * @param cost_millicents Cost in 1/1000 cents (may be NULL; non-zero only for OpenRouter)
 */
void llm_get_session_stats(uint32_t *in, uint32_t *out, uint32_t *cost_millicents);

/* ── Per-request override (for smart local/cloud routing) ─────── */

/** Override provider and model for the current request only.
 *  Call llm_clear_request_override() after the agent loop finishes. */
void llm_set_request_override(const char *provider, const char *model);

/** Clear per-request override, reverting to global provider/model. */
void llm_clear_request_override(void);

/** Set the local model name (e.g. "qwen3-vl:8b" — vision capable). */
void llm_set_local_model(const char *model);

/** Get the configured local model name (vision model when set). */
const char *llm_get_local_model(void);

/** Set the text-only local model (e.g. "gemma3:12b" — fast, no vision overhead). */
void llm_set_local_text_model(const char *model);

/** Get the text-only local model; falls back to local_model if not configured. */
const char *llm_get_local_text_model(void);

/** Check if the local Ollama instance is reachable (cached 15s). May block. */
bool llm_local_health_check(void);

/** Return last cached health check result (non-blocking). */
bool llm_local_is_online(void);

/** Returns true if smart routing is possible (cloud global + local configured). */
bool llm_smart_routing_available(void);

/** Set provider/model used for voice channel (chat_id=="ptt") requests.
 *  When set, voice bypasses local Ollama and uses a fast cloud model instead. */
void llm_set_voice_provider(const char *provider);
void llm_set_voice_model(const char *model);

/** Returns true if voice routing is configured (voice_provider + voice_model set). */
bool llm_voice_routing_available(void);

/** Get the configured voice provider/model strings. */
const char *llm_get_voice_provider(void);
const char *llm_get_voice_model(void);

/** Set/clear a reduced max_tokens for voice-channel requests (e.g. 400).
 *  When > 0, overrides LANG_LLM_MAX_TOKENS for the current request. */
void llm_set_voice_max_tokens(int n);
int  llm_get_voice_max_tokens(void);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct {
    char id[64];        /* "toolu_xxx" */
    char name[32];      /* "web_search" */
    char *input;        /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char *text;                                  /* accumulated text blocks */
    size_t text_len;
    llm_tool_call_t calls[LANG_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;                               /* stop_reason == "tool_use" */
    bool truncated;                              /* finish_reason == "length" (max_tokens hit) */
    uint32_t input_tokens;                       /* tokens consumed from context */
    uint32_t output_tokens;                      /* tokens generated */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
/**
 * force_tool_use: if true, set tool_choice to "any" (Anthropic) / "required"
 * (OpenAI) so the model MUST call at least one tool. Use on the first iteration
 * only when the user message is a known tool-trigger (e.g. "remember X").
 */
esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         bool force_tool_use,
                         llm_response_t *resp);

/* ── Streaming ─────────────────────────────────────────────────── */

/**
 * Called periodically during streaming with the accumulated text so far.
 * @param text  Full text accumulated up to this point (NUL-terminated).
 * @param len   Total bytes accumulated (same as strlen(text)).
 * @param ctx   Caller-supplied context pointer.
 */
typedef void (*llm_stream_progress_fn)(const char *text, size_t len, void *ctx);

/**
 * Like llm_chat_tools() but streams the response via SSE.
 * progress_cb is called as text accumulates (rate-limited internally).
 * Falls back to non-streaming automatically when HTTP proxy is enabled.
 * Returns the same llm_response_t format as llm_chat_tools().
 */
esp_err_t llm_chat_tools_streaming(const char *system_prompt,
                                   cJSON *messages,
                                   const char *tools_json,
                                   bool force_tool_use,
                                   llm_stream_progress_fn progress_cb,
                                   void *progress_ctx,
                                   llm_response_t *resp);
