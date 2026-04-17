#include "agent_loop.h"
#include "agent/context_builder.h"
#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "memory/psram_alloc.h"
#include "tools/tool_registry.h"
#include "gateway/ws_server.h"
#include "tools/tool_memory.h"
#include "tools/tool_web_search.h"
#include "tools/tool_smtp.h"
#include "tools/tool_notify.h"
#include "audio/tts_client.h"
#include "audio/i2s_audio.h"
#include "telegram/telegram_bot.h"
#include "led/led_indicator.h"
#include "display/oled_display.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "agent";
static _Atomic bool s_agent_busy = false;

/* ── Diagnostic state for external watchdog ─────────────────────── */
static _Atomic int64_t s_turn_start_us  = 0;   /* esp_timer_get_time() at turn start */
static char            s_phase[48]      = "";   /* current phase: "llm_cloud", "tool:weather", etc. */
static portMUX_TYPE    s_phase_mux      = portMUX_INITIALIZER_UNLOCKED;

static void agent_set_phase(const char *phase)
{
    taskENTER_CRITICAL(&s_phase_mux);
    strncpy(s_phase, phase, sizeof(s_phase) - 1);
    s_phase[sizeof(s_phase) - 1] = '\0';
    taskEXIT_CRITICAL(&s_phase_mux);
}

void agent_loop_get_phase(char *buf, size_t len)
{
    taskENTER_CRITICAL(&s_phase_mux);
    strncpy(buf, s_phase, len - 1);
    buf[len - 1] = '\0';
    taskEXIT_CRITICAL(&s_phase_mux);
}

int64_t agent_loop_turn_age_ms(void)
{
    int64_t start = atomic_load(&s_turn_start_us);
    if (start == 0) return 0;   /* not in a turn */
    return (esp_timer_get_time() - start) / 1000;
}

/* ── LLM API rate limiting ────────────────────────────────────── */
/* Tracks hourly request count. Resets every hour.
 * Default: 60/hour. Configurable via CLI: rate_limit <n>  */
#define RATE_LIMIT_DEFAULT_PER_HOUR  60
#define RATE_LIMIT_WINDOW_US         (3600LL * 1000000LL)  /* 1 hour */

static int      s_rate_limit_max  = RATE_LIMIT_DEFAULT_PER_HOUR;
static int      s_rate_limit_count = 0;
static int64_t  s_rate_limit_window_start = 0;

static bool agent_rate_limit_ok(void)
{
    int64_t now = esp_timer_get_time();
    /* Reset window if expired */
    if (now - s_rate_limit_window_start > RATE_LIMIT_WINDOW_US) {
        s_rate_limit_count = 0;
        s_rate_limit_window_start = now;
    }
    if (s_rate_limit_count >= s_rate_limit_max) {
        return false;
    }
    s_rate_limit_count++;
    return true;
}

void agent_set_rate_limit(int max_per_hour)
{
    if (max_per_hour < 1) max_per_hour = 1;
    if (max_per_hour > 999) max_per_hour = 999;
    s_rate_limit_max = max_per_hour;
    ESP_LOGI(TAG, "LLM rate limit set to %d/hour", max_per_hour);
}

int agent_get_rate_limit(void)
{
    return s_rate_limit_max;
}

int agent_get_rate_count(void)
{
    /* If window expired, return 0 */
    if (esp_timer_get_time() - s_rate_limit_window_start > RATE_LIMIT_WINDOW_US) {
        return 0;
    }
    return s_rate_limit_count;
}

#define TOOL_OUTPUT_SIZE      (32 * 1024)
#define WS_TOKEN_MIN_CHARS    8
#define WS_TOKEN_MIN_US       150000

/* Progress callback: sends token deltas to the WS client */
typedef struct {
    size_t   last_sent_len;
    uint64_t last_sent_us;
    char     chat_id[32];
    char     channel[16];
} ws_stream_ctx_t;

static void ws_stream_progress(const char *text, size_t len, void *ctx)
{
    ws_stream_ctx_t *sc = (ws_stream_ctx_t *)ctx;
    if (!sc || !sc->chat_id[0]) return;

    /* Only stream WebSocket channels — Telegram uses placeholder/edit */
    if (strcmp(sc->channel, "websocket") != 0) return;

    if (len <= sc->last_sent_len) return;

    const char *delta    = text + sc->last_sent_len;
    size_t      delta_len = len - sc->last_sent_len;

    /* Gate: avoid flooding with tiny fragments */
    uint64_t now_us   = (uint64_t)esp_timer_get_time();
    bool enough_chars = delta_len >= WS_TOKEN_MIN_CHARS;
    bool enough_time  = (now_us - sc->last_sent_us) >= WS_TOKEN_MIN_US;
    if (!enough_chars && !enough_time) return;

    ws_server_send_token(sc->chat_id, delta);
    sc->last_sent_len = len;
    sc->last_sent_us  = now_us;

    ws_server_broadcast_monitor("llm", delta);
}

/**
 * Find the end index of the first sentence in `text`, suitable for splitting
 * TTS generation across two segments. Segment A (0..return) is sent to TTS
 * first so playback starts while the rest of the message is still synthesizing.
 *
 * Constraints:
 *   - sentence terminator (. ! ? \n\n) must appear between [min_chars, max_chars]
 *   - at least `min_tail` chars must remain after the split (otherwise not worth it)
 *
 * Returns 0 when no suitable boundary is found — caller should fall back to
 * the single-shot TTS path.
 */
static size_t find_first_sentence_end(const char *text, size_t min_chars,
                                       size_t max_chars, size_t min_tail)
{
    if (!text) return 0;
    size_t len = strlen(text);
    if (len < min_chars + min_tail) return 0;
    size_t scan_end = (len > min_tail) ? (len - min_tail) : 0;
    if (scan_end > max_chars) scan_end = max_chars;
    for (size_t i = min_chars; i < scan_end; i++) {
        char c = text[i];
        char nxt = text[i + 1];
        if ((c == '.' || c == '!' || c == '?') &&
            (nxt == ' ' || nxt == '\n' || nxt == '\t' || nxt == '\0')) {
            return i + 1;
        }
        if (c == '\n' && nxt == '\n') {
            return i;
        }
    }
    return 0;
}

static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) return;
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const lang_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) return;

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) return;

    bool is_voice = (strcmp(msg->chat_id, "ptt") == 0);

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "%s",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)",
        is_voice
            ? "- VOICE MODE: This response will be spoken aloud. "
              "Use natural conversational English only — no markdown, no bullet points, "
              "no headers, no asterisks, no backticks. "
              "Keep the reply under 3 sentences unless the user asks for more detail.\n"
            : "");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const lang_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) return NULL;

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) return NULL;

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        changed = true;
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

/* ── Parallel tool execution ──────────────────────────────────── */

typedef struct {
    const llm_tool_call_t *call;
    const char            *input;       /* points into patched or call->input */
    char                  *output;      /* ps_malloc(TOOL_OUTPUT_SIZE) */
    TaskHandle_t           notify_task;
} tool_exec_ctx_t;

static void tool_exec_task(void *arg)
{
    tool_exec_ctx_t *ctx = (tool_exec_ctx_t *)arg;
    tool_registry_execute(ctx->call->name, ctx->input, ctx->output, TOOL_OUTPUT_SIZE);
    xTaskNotifyGive(ctx->notify_task);
    vTaskDelete(NULL);
}

static cJSON *build_tool_results(const llm_response_t *resp, const lang_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();
    int n = resp->call_count;

    /* Parallel path: run independent tool calls concurrently.
     * Guard: each task needs ~16KB SRAM stack + ~12KB TLS heap headroom.
     * If free SRAM is insufficient, fall through to the sequential path. */
    if (n > 1) {
        tool_exec_ctx_t ctxs[LANG_MAX_TOOL_CALLS];
        char *patched[LANG_MAX_TOOL_CALLS];
        bool all_alloc = true;
        int tasks_spawned = 0;
        TaskHandle_t self = xTaskGetCurrentTaskHandle();

        /* SRAM guard: (16KB stack + 12KB TLS) per task + 28KB safety floor */
        uint32_t free_sram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t par_needed = (uint32_t)n * (16384u + 12288u) + 28672u;
        if (free_sram < par_needed) {
            char wmsg[96];
            snprintf(wmsg, sizeof(wmsg),
                     "tool par: SRAM %lu B < %lu needed — sequential fallback",
                     (unsigned long)free_sram, (unsigned long)par_needed);
            ESP_LOGW(TAG, "%s", wmsg);
            ws_server_broadcast_monitor("task", wmsg);
            all_alloc = false;  /* skip parallel, drop into sequential below */
        }

        memset(ctxs,    0, n * sizeof(tool_exec_ctx_t));
        memset(patched, 0, n * sizeof(char *));

        /* Allocate per-call PSRAM output buffers (only if SRAM guard passed) */
        if (all_alloc) {
        for (int i = 0; i < n; i++) {
            ctxs[i].output = ps_malloc(TOOL_OUTPUT_SIZE);
            if (!ctxs[i].output) {
                /* Free any already-allocated buffers before falling back */
                for (int j = 0; j < i; j++) { free(ctxs[j].output); ctxs[j].output = NULL; }
                all_alloc = false;
                break;
            }
            ctxs[i].output[0] = '\0';
        }
        }

        if (all_alloc) {
            /* Spawn a task per tool call */
            for (int i = 0; i < n; i++) {
                const llm_tool_call_t *call = &resp->calls[i];
                patched[i]          = patch_tool_input_with_context(call, msg);
                ctxs[i].call        = call;
                ctxs[i].input       = patched[i] ? patched[i] :
                                      (call->input ? call->input : "{}");
                ctxs[i].notify_task = self;

                {
                    char mon[128];
                    snprintf(mon, sizeof(mon), "%s %.80s", call->name, ctxs[i].input);
                    for (char *p = mon; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                    ws_server_broadcast_monitor("tool", mon);
                    /* OLED: show tool being called */
                    char oled_tool[64];
                    snprintf(oled_tool, sizeof(oled_tool), "Tool: %s", call->name);
                    oled_display_set_message(oled_tool);
                }

                if (xTaskCreate(tool_exec_task, "tool_par", 16384,
                                &ctxs[i], 5, NULL) == pdPASS) {
                    tasks_spawned++;
                } else {
                    ESP_LOGW(TAG, "tool_par create failed, running inline");
                    tool_registry_execute(call->name, ctxs[i].input,
                                          ctxs[i].output, TOOL_OUTPUT_SIZE);
                }
            }

            /* Wait for all spawned tasks — 25s per task (below 30s WDT) */
            for (int i = 0; i < tasks_spawned; i++) {
                if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25000))) {
                    ESP_LOGW(TAG, "tool_par[%d] timed out after 25s", i);
                    ws_server_broadcast_monitor("error", "tool_par timeout (25s) — partial results");
                    /* Fill empty results with error so LLM knows what failed
                     * instead of seeing an empty string and retrying blindly */
                    if (i < n && ctxs[i].output && ctxs[i].output[0] == '\0') {
                        snprintf(ctxs[i].output, TOOL_OUTPUT_SIZE,
                                 "Error: tool execution timed out after 25 seconds");
                    }
                }
            }

            /* Collect results in call order */
            for (int i = 0; i < n; i++) {
                const llm_tool_call_t *call = &resp->calls[i];
                const char *out = ctxs[i].output;
                int out_len = (int)strlen(out);
                ESP_LOGI(TAG, "Tool [par] %s: %d bytes", call->name, out_len);

                if (strncmp(out, "Error:", 6) == 0) {
                    char emsg[160];
                    snprintf(emsg, sizeof(emsg), "[%s] %.100s", call->name, out);
                    ws_server_broadcast_monitor("error", emsg);
                } else {
                    char preview[128];
                    snprintf(preview, sizeof(preview), "[%s] %d bytes: %.40s%s",
                             call->name, out_len, out, out_len > 40 ? "..." : "");
                    for (char *p = preview; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                    ws_server_broadcast_monitor("tool", preview);
                }

                cJSON *rb = cJSON_CreateObject();
                cJSON_AddStringToObject(rb, "type", "tool_result");
                cJSON_AddStringToObject(rb, "tool_use_id", call->id);
                cJSON_AddStringToObject(rb, "content", out);
                cJSON_AddItemToArray(content, rb);

                free(ctxs[i].output);
                free(patched[i]);
            }
            return content;
        }

        /* OOM: free any allocated buffers and fall through to sequential */
        for (int i = 0; i < n; i++) { free(ctxs[i].output); }
        ESP_LOGW(TAG, "Parallel tool PSRAM alloc failed, falling back to sequential");
    }

    /* Sequential path (n==1 or OOM fallback) */
    for (int i = 0; i < n; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) tool_input = patched_input;

        tool_output[0] = '\0';
        {
            char tool_mon[128];
            snprintf(tool_mon, sizeof(tool_mon), "%s %.80s", call->name, tool_input);
            for (char *p = tool_mon; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("tool", tool_mon);
            /* OLED: show tool being called */
            char oled_tool[64];
            snprintf(oled_tool, sizeof(oled_tool), "Tool: %s", call->name);
            oled_display_set_message(oled_tool);
        }
        int64_t t0 = esp_timer_get_time();
        /* Watchdog phase: sequential tool name for diagnostics */
        {
            char pb[48];
            snprintf(pb, sizeof(pb), "tool:%.42s", call->name);
            agent_set_phase(pb);
        }
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
        int tool_ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (tool_ms > 30000) {
            ESP_LOGW(TAG, "Tool %s took %d ms (sequential) — unusually slow",
                     call->name, tool_ms);
        }
        free(patched_input);

        int tool_out_len = (int)strlen(tool_output);
        ESP_LOGI(TAG, "Tool %s result: %d bytes in %dms", call->name, tool_out_len, tool_ms);

        if (strncmp(tool_output, "Error:", 6) == 0) {
            char emsg[160];
            snprintf(emsg, sizeof(emsg), "[%s] %.100s (%dms)", call->name, tool_output, tool_ms);
            ws_server_broadcast_monitor("error", emsg);
        } else {
            char preview[128];
            snprintf(preview, sizeof(preview), "[%s] %d bytes (%dms): %.40s%s",
                     call->name, tool_out_len, tool_ms, tool_output,
                     tool_out_len > 40 ? "..." : "");
            for (char *p = preview; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("tool", preview);
        }

        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large persistent buffers in PSRAM */
    char *system_prompt = ps_calloc(1, LANG_CONTEXT_BUF_SIZE);  /* 32KB from PSRAM */
    char *tool_output   = ps_calloc(1, TOOL_OUTPUT_SIZE);        /* 16KB from PSRAM */

    if (!system_prompt || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate agent buffers");
        free(system_prompt);
        free(tool_output);
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        lang_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        atomic_store(&s_agent_busy, true);
        int64_t turn_start_us = esp_timer_get_time();
        atomic_store(&s_turn_start_us, turn_start_us);
        agent_set_phase("init");
        bool turn_has_image = false;   /* set when tool_capture_image runs this turn */
        memory_tool_reset_turn();
        web_search_reset_turn();

        /* SRAM guard: restart only if genuinely unable to complete a turn.
         * Empirical floor: heap_min reached 13.9 KB during a 7-search briefing
         * and completed successfully.  mbedTLS session-ticket cache (held after
         * multiple HTTPS connections) consumes ~8-10 KB persistently — this is
         * normal and does NOT require a restart.
         *
         * Thresholds:
         *   < 14 KB  — hard restart (below observed safe floor)
         *   < 24 KB  — warning only, proceed
         *
         * A hard restart shows as ESP_RST_SW (intentional), not ESP_RST_PANIC. */
        {
            uint32_t sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            ESP_LOGI(TAG, "Turn start: SRAM free=%lu B", (unsigned long)sram_free);
            if (sram_free < 14 * 1024) {
                /* Try lighter recovery first: trim session to free cJSON memory. */
                ESP_LOGW(TAG, "SRAM critically low (%lu B) — trimming session %s",
                         (unsigned long)sram_free, msg.chat_id);
                session_trim(msg.chat_id, 10);
                session_cache_invalidate(msg.chat_id);

                uint32_t sram_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                ESP_LOGI(TAG, "SRAM after session trim: %lu B (was %lu B)",
                         (unsigned long)sram_after, (unsigned long)sram_free);

                if (sram_after < 14 * 1024) {
                    char wmsg[80];
                    snprintf(wmsg, sizeof(wmsg),
                             "SRAM still critically low (%lu B) after trim — restarting",
                             (unsigned long)sram_after);
                    ESP_LOGW(TAG, "%s", wmsg);
                    ws_server_broadcast_monitor("system", wmsg);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
                ws_server_broadcast_monitor("system", "SRAM recovered via session trim");
            } else if (sram_free < 24 * 1024) {
                char wmsg[72];
                snprintf(wmsg, sizeof(wmsg), "SRAM low at turn start: %lu B — proceeding",
                         (unsigned long)sram_free);
                ESP_LOGW(TAG, "%s", wmsg);
                ws_server_broadcast_monitor("system", wmsg);
            }
        }

        /* LLM API rate limiting — skip system messages (cron, heartbeat) which
         * are self-limiting by their own schedules. Only limit user-initiated. */
        if (strcmp(msg.channel, LANG_CHAN_SYSTEM) != 0 && !agent_rate_limit_ok()) {
            ESP_LOGW(TAG, "LLM rate limit reached (%d/%d per hour) for %s",
                     s_rate_limit_count, s_rate_limit_max, msg.chat_id);
            ws_server_broadcast_monitor("system", "LLM rate limit reached");

            /* Send a response instead of calling the LLM */
            char rate_msg[128];
            snprintf(rate_msg, sizeof(rate_msg),
                     "Rate limit reached (%d requests/hour). Try again in a few minutes.",
                     s_rate_limit_max);

            lang_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup(rate_msg);
            if (out.content) {
                message_bus_push_outbound(&out);
            }
            free(msg.content);
            atomic_store(&s_agent_busy, false);
            continue;
        }

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        {
            char mon[64];
            snprintf(mon, sizeof(mon), "from %s:%s", msg.channel, msg.chat_id);
            ws_server_broadcast_monitor("task", mon);
        }

        /* OLED: set channel + show incoming message preview */
        {
            const char *ch = msg.channel;
            const char *abbr = "?";
            if      (strcmp(ch, "websocket") == 0) abbr = "WS";
            else if (strcmp(ch, "telegram")  == 0) abbr = "TG";
            else if (strcmp(ch, "system")    == 0) abbr = "SYS";
            else if (strcmp(ch, "ptt") == 0 || strcmp(msg.chat_id, "ptt") == 0) abbr = "PTT";
            oled_display_set_channel(abbr);
        }
        {
            char oled_msg[64];
            snprintf(oled_msg, sizeof(oled_msg), "> %.58s", msg.content ? msg.content : "");
            oled_display_set_message(oled_msg);
        }

        /* 1. Build system prompt in PSRAM buffer */
        context_build_system_prompt(system_prompt, LANG_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, LANG_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history */
        cJSON *messages = session_get_history_cjson(msg.chat_id, LANG_AGENT_MAX_HISTORY);

        /* Content-byte budget: drop oldest user/assistant pairs to fit */
        {
            int total_chars = 0;
            int arr_size = cJSON_GetArraySize(messages);
            for (int i = 0; i < arr_size; i++) {
                cJSON *m = cJSON_GetArrayItem(messages, i);
                cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
                if (c && cJSON_IsString(c))
                    total_chars += (int)strlen(c->valuestring);
            }
            int dropped = 0;
            int drop_limit = arr_size;
            while (dropped < drop_limit &&
                   total_chars > LANG_SESSION_HISTORY_MAX_BYTES &&
                   cJSON_GetArraySize(messages) >= 2) {
                cJSON *oldest = cJSON_GetArrayItem(messages, 0);
                cJSON *c = cJSON_GetObjectItemCaseSensitive(oldest, "content");
                if (c && cJSON_IsString(c))
                    total_chars -= (int)strlen(c->valuestring);
                cJSON_DeleteItemFromArray(messages, 0);
                dropped++;
            }
            if (dropped > 0) {
                char vmsg[64];
                snprintf(vmsg, sizeof(vmsg), "history trimmed: dropped %d msgs", dropped);
                ws_server_broadcast_monitor_verbose("task", vmsg);
            }
        }

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;
        bool recovery_tried = false;
        bool retry_done = false;
        bool oom_restart = false;
        bool capture_image_called = false;
        bool say_tool_used = false;
        esp_err_t last_err = ESP_OK;
        bool is_telegram = (strcmp(msg.channel, LANG_CHAN_TELEGRAM) == 0);
        int32_t tg_placeholder_id = -1;

        /* Telegram: send placeholder before LLM call */
        if (is_telegram) {
            tg_placeholder_id = telegram_send_get_id(msg.chat_id, "🤔 thinking...");
        }

        /* Send status: thinking */
        ws_server_send_status(msg.chat_id, "llm_thinking");

        /* Detect memory trigger keywords */
        bool force_memory_tool = false;
        {
            const char *txt = msg.content ? msg.content : "";
            char lower[64] = {0};
            size_t tlen = strlen(txt);
            for (size_t ci = 0; ci < tlen && ci < sizeof(lower) - 1; ci++)
                lower[ci] = (char)tolower((unsigned char)txt[ci]);
            if (strstr(lower, "remember") || strstr(lower, "save that") ||
                strstr(lower, "note that") || strstr(lower, "don't forget") ||
                strstr(lower, "dont forget") || strstr(lower, "make a note") ||
                strstr(lower, "keep in mind")) {
                force_memory_tool = true;
                ws_server_broadcast_monitor_verbose("task", "memory trigger detected");
            }
        }

        ws_stream_ctx_t stream_ctx = { .last_sent_len = 0, .last_sent_us = 0 };
        strncpy(stream_ctx.chat_id, msg.chat_id, sizeof(stream_ctx.chat_id) - 1);
        strncpy(stream_ctx.channel, msg.channel, sizeof(stream_ctx.channel) - 1);

        /* Smart routing: unified Apfel → Ollama → cloud hierarchy.
         * Both voice and text channels use the same tiered approach.
         * System/heartbeat always uses cloud (multi-step tool chains).
         * Voice adds max_tokens=400 cap and VOICE MODE prompt injection. */
        bool using_local        = false;
        bool using_apfel        = false;
        bool using_voice_cloud  = false;
        bool is_voice           = (strcmp(msg.chat_id, "ptt") == 0);

        if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0) {
            /* System/heartbeat/cron → always cloud (complex multi-tool chains) */
            ESP_LOGI(TAG, "Smart routing: system channel → cloud");
            ws_server_broadcast_monitor("llm", "routing: cloud (system)");
        } else if (is_voice || llm_smart_routing_available()) {
            /* Both voice and text: Apfel → Ollama → cloud hierarchy.
             *
             * Complexity check: route complex requests (web search, email, briefings)
             * to cloud — local models struggle with long tool chains. */
            bool is_complex = (strcasestr(msg.content, "briefing")  != NULL ||
                               strcasestr(msg.content, "web search") != NULL ||
                               strcasestr(msg.content, "search for") != NULL ||
                               strcasestr(msg.content, " email")     != NULL ||
                               strcasestr(msg.content, "send email") != NULL ||
                               strcasestr(msg.content, "research")   != NULL);

            /* Requests needing tools bypass Apfel (4K context can't hold tool schemas).
             * Keyword triggers fall into three buckets:
             *  - Explicit tool calls (weather, stock, remind, note, device/system info)
             *  - Temporal markers (today, tonight, this week[end], latest, current, now)
             *    → the user wants CURRENT information Apfel doesn't have
             *  - Current-events phrasing (what's on, what's happening, events, news,
             *    score, price of, how much, near me) → needs web_search */
            bool needs_tools = is_complex ||
                               /* Explicit tool intents */
                               (strcasestr(msg.content, "weather")     != NULL) ||
                               (strcasestr(msg.content, "forecast")    != NULL) ||
                               (strcasestr(msg.content, "what time")   != NULL) ||
                               (strcasestr(msg.content, "remind")      != NULL) ||
                               (strcasestr(msg.content, "cron")        != NULL) ||
                               (strcasestr(msg.content, "remember")    != NULL) ||
                               (strcasestr(msg.content, "note that")   != NULL) ||
                               (strcasestr(msg.content, "stock")       != NULL) ||
                               (strcasestr(msg.content, "device")      != NULL) ||
                               (strcasestr(msg.content, "system")      != NULL) ||
                               /* Temporal markers → user wants current info */
                               (strcasestr(msg.content, "today")       != NULL) ||
                               (strcasestr(msg.content, "tonight")     != NULL) ||
                               (strcasestr(msg.content, "this week")   != NULL) ||
                               (strcasestr(msg.content, "weekend")     != NULL) ||
                               (strcasestr(msg.content, "latest")      != NULL) ||
                               (strcasestr(msg.content, "current")     != NULL) ||
                               (strcasestr(msg.content, "right now")   != NULL) ||
                               /* Current-events phrasing → web_search */
                               (strcasestr(msg.content, "what's on")   != NULL) ||
                               (strcasestr(msg.content, "whats on")    != NULL) ||
                               (strcasestr(msg.content, "happening")   != NULL) ||
                               (strcasestr(msg.content, "events")      != NULL) ||
                               (strcasestr(msg.content, "concerts")    != NULL) ||
                               (strcasestr(msg.content, "news")        != NULL) ||
                               (strcasestr(msg.content, "score")       != NULL) ||
                               (strcasestr(msg.content, "who won")     != NULL) ||
                               (strcasestr(msg.content, "price of")    != NULL) ||
                               (strcasestr(msg.content, "how much is") != NULL) ||
                               (strcasestr(msg.content, "near me")     != NULL) ||
                               force_memory_tool || turn_has_image;

            if (is_complex) {
                /* Complex → cloud directly */
                if (is_voice) llm_set_voice_max_tokens(400);
                ESP_LOGI(TAG, "Smart routing: complex request → cloud");
                ws_server_broadcast_monitor("llm", "routing: cloud (complex)");
            } else if (!needs_tools && llm_apfel_health_check()) {
                /* Apfel: Apple Foundation Model — ultra-fast, no tools, minimal context.
                 * Best tier for simple conversational queries (voice or text). */
                llm_set_request_override("apfel", llm_get_apfel_model());
                using_apfel = true;
                if (is_voice) llm_set_voice_max_tokens(400);
                /* Rebuild system prompt with minimal version for 4K context window */
                context_build_minimal_prompt(system_prompt, LANG_CONTEXT_BUF_SIZE);
                /* Aggressively trim history for Apfel's small context */
                {
                    int total_chars = 0;
                    int arr_size = cJSON_GetArraySize(messages);
                    for (int i = 0; i < arr_size; i++) {
                        cJSON *m = cJSON_GetArrayItem(messages, i);
                        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
                        if (c && cJSON_IsString(c))
                            total_chars += (int)strlen(c->valuestring);
                    }
                    while (total_chars > 2048 && cJSON_GetArraySize(messages) >= 2) {
                        cJSON *oldest = cJSON_GetArrayItem(messages, 0);
                        cJSON *c = cJSON_GetObjectItemCaseSensitive(oldest, "content");
                        if (c && cJSON_IsString(c))
                            total_chars -= (int)strlen(c->valuestring);
                        cJSON_DeleteItemFromArray(messages, 0);
                    }
                }
                {
                    char route_msg[64];
                    snprintf(route_msg, sizeof(route_msg), "routing: apfel → %s",
                             llm_get_apfel_model());
                    ESP_LOGI(TAG, "Smart routing: %s", route_msg);
                    ws_server_broadcast_monitor("llm", route_msg);
                }
            } else if (llm_local_health_check()) {
                /* Local LLM (Ollama or mlx-lm): full tool support, larger context */
                const char *local_model = turn_has_image
                    ? llm_get_local_model()
                    : llm_get_local_text_model();
                llm_set_request_override(llm_get_local_provider(), local_model);
                using_local = true;
                if (is_voice) llm_set_voice_max_tokens(400);
                {
                    char route_msg[64];
                    snprintf(route_msg, sizeof(route_msg), "routing: local → %s%s",
                             local_model, turn_has_image ? " (vision)" : "");
                    ESP_LOGI(TAG, "Smart routing: %s", route_msg);
                    ws_server_broadcast_monitor("llm", route_msg);
                }
            } else if (is_voice && llm_voice_routing_available()) {
                /* Cloud fallback for voice — use dedicated voice model */
                llm_set_request_override(llm_get_voice_provider(), llm_get_voice_model());
                llm_set_voice_max_tokens(400);
                using_voice_cloud = true;
                {
                    char route_msg[64];
                    snprintf(route_msg, sizeof(route_msg), "routing: cloud (voice fallback) → %s/%s",
                             llm_get_voice_provider(), llm_get_voice_model());
                    ESP_LOGI(TAG, "Smart routing: %s", route_msg);
                    ws_server_broadcast_monitor("llm", route_msg);
                }
            } else {
                ESP_LOGI(TAG, "Smart routing: local offline, using cloud");
                ws_server_broadcast_monitor("llm", "routing: cloud (local offline)");
            }
            /* Push local service status to OLED */
            oled_display_set_local_status(llm_local_is_online(),
                                          tts_local_is_online(),
                                          llm_apfel_is_online());
        }

        while (iteration < LANG_AGENT_MAX_TOOL_ITER) {
            /* Soft per-turn timeout. System tasks get 5 min (briefing involves
             * 3-4 cloud LLM calls + web_search + email). Interactive turns get
             * 3 min — enough for tool chains while giving the user feedback. */
            int64_t turn_timeout_us = (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0)
                ? 300000000LL   /* 5 min for heartbeat/cron */
                : 180000000LL;  /* 3 min for browser/Telegram */
            if ((esp_timer_get_time() - turn_start_us) > turn_timeout_us) {
                ESP_LOGW(TAG, "Agent soft timeout at iter %d — abandoning turn", iteration);
                ws_server_broadcast_monitor("error", "agent turn timeout");
                final_text = strdup("[Sorry, that took too long. Please retry.]");
                break;
            }

#if LANG_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, LANG_CHAN_SYSTEM) != 0 && !is_telegram) {
                lang_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("Langoustine is thinking...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != ESP_OK) {
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif

            {
                uint32_t sram_iter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                char itermsg[80];
                snprintf(itermsg, sizeof(itermsg), "LLM iter %d — SRAM %luKB free",
                         iteration + 1, (unsigned long)(sram_iter / 1024));
                ESP_LOGI(TAG, "%s", itermsg);
                ws_server_broadcast_monitor("llm", itermsg);
            }
            led_indicator_set(LED_THINKING);
            /* Show provider/model on OLED before LLM call */
            {
                char prov_line[32];
                snprintf(prov_line, sizeof(prov_line), "%.15s/%.14s",
                         llm_get_provider(), llm_get_model());
                oled_display_set_provider(prov_line);
                /* Watchdog phase: track which LLM tier we're calling */
                char phase_buf[48];
                snprintf(phase_buf, sizeof(phase_buf), "llm:%.42s", llm_get_provider());
                agent_set_phase(phase_buf);
            }
            llm_response_t resp;
            bool force_this_iter = (force_memory_tool && iteration == 0);
            const char *effective_tools = using_apfel ? NULL : tools_json;
            err = llm_chat_tools_streaming(system_prompt, messages, effective_tools,
                                           force_this_iter && !using_apfel,
                                           ws_stream_progress, &stream_ctx,
                                           &resp);

            if (err != ESP_OK && !retry_done) {
                retry_done = true;
                {
                    char rmsg[80];
                    snprintf(rmsg, sizeof(rmsg), "LLM: %s — retrying...", esp_err_to_name(err));
                    ws_server_broadcast_monitor("error", rmsg);
                }
                vTaskDelay(pdMS_TO_TICKS(250));  /* brief pause, then retry */
                err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                               force_this_iter,
                                               ws_stream_progress, &stream_ctx,
                                               &resp);
            }

            /* Apfel fallback: if Apfel failed, try Ollama with full context */
            if (err != ESP_OK && using_apfel) {
                ESP_LOGW(TAG, "Apfel failed (%s), trying local LLM", esp_err_to_name(err));
                ws_server_broadcast_monitor("llm", "apfel failed — trying local LLM");
                using_apfel = false;
                if (llm_local_health_check()) {
                    const char *local_model = llm_get_local_text_model();
                    llm_set_request_override(llm_get_local_provider(), local_model);
                    using_local = true;
                    /* Rebuild full context for Ollama (Apfel had minimal) */
                    context_build_system_prompt(system_prompt, LANG_CONTEXT_BUF_SIZE);
                    retry_done = false;
                    err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                                   force_this_iter,
                                                   ws_stream_progress, &stream_ctx,
                                                   &resp);
                } else {
                    /* Neither Apfel nor Ollama available — fall through to cloud */
                    llm_clear_request_override();
                    context_build_system_prompt(system_prompt, LANG_CONTEXT_BUF_SIZE);
                    retry_done = false;
                    err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                                   force_this_iter,
                                                   ws_stream_progress, &stream_ctx,
                                                   &resp);
                }
            }

            /* Local model fallback: if text model failed (e.g. gemma3 doesn't
             * support tools), retry with the primary local model which does
             * (e.g. qwen3-vl).  Only if they're actually different models. */
            if (err != ESP_OK && using_local) {
                const char *primary = llm_get_local_model();
                const char *text_m  = llm_get_local_text_model();
                if (!turn_has_image && strcmp(primary, text_m) != 0) {
                    ESP_LOGW(TAG, "Local text model failed (%s), trying primary local: %s",
                             esp_err_to_name(err), primary);
                    ws_server_broadcast_monitor("llm", "text model failed — trying primary local");
                    oled_display_set_message(primary);
                    llm_set_request_override(llm_get_local_provider(), primary);
                    retry_done = false;
                    err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                                   force_this_iter,
                                                   ws_stream_progress, &stream_ctx,
                                                   &resp);
                }
            }

            /* Cloud fallback: if local Ollama failed, retry with global cloud provider */
            if (err != ESP_OK && using_local) {
                ESP_LOGW(TAG, "Local LLM failed (%s), falling back to cloud", esp_err_to_name(err));
                ws_server_broadcast_monitor("llm", "local failed — falling back to cloud");
                oled_display_set_message("Fallback: cloud");
                llm_clear_request_override();
                using_local = false;
                retry_done = false;
                err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                               force_this_iter,
                                               ws_stream_progress, &stream_ctx,
                                               &resp);
            }

            /* Voice cloud fallback: if the dedicated voice model failed, clear override
             * and retry with the global cloud provider */
            if (err != ESP_OK && using_voice_cloud) {
                ESP_LOGW(TAG, "Voice cloud LLM failed (%s), falling back to global provider",
                         esp_err_to_name(err));
                ws_server_broadcast_monitor("llm", "voice cloud failed — falling back to global");
                llm_clear_request_override();
                llm_set_voice_max_tokens(0);
                using_voice_cloud = false;
                retry_done = false;
                err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                               force_this_iter,
                                               ws_stream_progress, &stream_ctx,
                                               &resp);
            }

            if (err != ESP_OK) {
                char emsg[80];
                snprintf(emsg, sizeof(emsg), "LLM failed: %s", esp_err_to_name(err));
                ESP_LOGE(TAG, "%s", emsg);
                ws_server_broadcast_monitor("error", emsg);
                oled_display_set_message(emsg);
                last_err = err;
                if (err == ESP_ERR_NO_MEM) { oom_restart = true; }
                break;
            }

            /* Update OLED with session token counts + rate limit */
            {
                uint32_t in_tok, out_tok;
                llm_get_session_stats(&in_tok, &out_tok, NULL);
                oled_display_set_tokens(in_tok, out_tok);

                char rl[22];
                snprintf(rl, sizeof(rl), "%d/%d req/hr",
                         agent_get_rate_count(), agent_get_rate_limit());
                oled_display_set_rotate_line(3, rl);
            }

            /* Heartbeat bloat warning — the ReAct loop accumulates tool results
             * in messages[], so a multi-step heartbeat can balloon from ~5 KB to
             * 30+ KB across iterations. This warning makes the pathology visible
             * without changing routing logic (Pico Phase A will do real trimming). */
            if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0 &&
                strcmp(msg.chat_id, "heartbeat") == 0 &&
                ((iteration + 1) >= LANG_HEARTBEAT_WARN_ITERS ||
                 resp.input_tokens >= LANG_HEARTBEAT_WARN_INPUT_TOK)) {
                char wmsg[96];
                snprintf(wmsg, sizeof(wmsg),
                         "heartbeat bloat: iter %d/%d, %lu in-tokens this call",
                         iteration + 1, LANG_AGENT_MAX_TOOL_ITER,
                         (unsigned long)resp.input_tokens);
                ESP_LOGW(TAG, "%s", wmsg);
                ws_server_broadcast_monitor("warn", wmsg);
                oled_display_set_rotate_line(1, "ctx bloat hb");
            }

            if (!resp.tool_use) {
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    if (!final_text) {
                        /* Log BEFORE freeing resp so text_len is still valid */
                        ESP_LOGE(TAG, "strdup OOM for final_text (%d bytes)", (int)resp.text_len);
                        oom_restart = true;
                    }
                    llm_response_free(&resp);
                    break;
                }
                /* Recovery: retry once if truncated OR if tools ran but LLM
                 * returned 0 bytes of final text (stop=end_turn with no content
                 * after multi-iteration tool use — common with daily briefing). */
                bool do_recovery = !recovery_tried &&
                                   (resp.truncated || iteration > 0);
                llm_response_free(&resp);
                if (do_recovery) {
                    recovery_tried = true;
                    iteration++;
                    const char *reason = (iteration <= 1)
                        ? "LLM: truncated, injecting recovery"
                        : "LLM: empty response after tools, injecting recovery";
                    ws_server_broadcast_monitor("error", reason);
                    ESP_LOGW(TAG, "%s (iter %d)", reason, iteration);
                    cJSON *recovery = cJSON_CreateObject();
                    cJSON_AddStringToObject(recovery, "role", "user");
                    cJSON_AddStringToObject(recovery, "content",
                        "Your response was cut off. Please give a brief, direct text answer now. Do not use any tools.");
                    cJSON_AddItemToArray(messages, recovery);
                    continue;
                }
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);
            /* OLED: show tool name(s) being called */
            {
                char oled_tool[128];
                if (resp.call_count == 1) {
                    snprintf(oled_tool, sizeof(oled_tool), "Tool: %s", resp.calls[0].name);
                } else {
                    int off = snprintf(oled_tool, sizeof(oled_tool), "Tools: ");
                    for (int ti = 0; ti < resp.call_count && off < (int)sizeof(oled_tool) - 2; ti++) {
                        off += snprintf(oled_tool + off, sizeof(oled_tool) - off, "%s%s",
                                        ti > 0 ? ", " : "", resp.calls[ti].name);
                    }
                }
                oled_display_set_status(oled_tool);
                char iter_msg[32];
                snprintf(iter_msg, sizeof(iter_msg), "Iter %d/%d",
                         iteration + 1, LANG_AGENT_MAX_TOOL_ITER);
                oled_display_set_message(iter_msg);
            }

            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Track if capture_image was invoked this turn; flash LED white for illumination.
             * Also set turn_has_image so subsequent LLM routing uses the vision model. */
            for (int ci = 0; ci < resp.call_count; ci++) {
                if (strcmp(resp.calls[ci].name, "capture_image") == 0) {
                    capture_image_called = true;
                    turn_has_image = true;
                    led_indicator_set(LED_CAPTURING);
                    break;
                }
            }

            /* Check if say tool was called — it plays audio immediately,
             * so we should exit the loop and skip final TTS generation. */
            for (int ci = 0; ci < resp.call_count; ci++) {
                if (strcmp(resp.calls[ci].name, "say") == 0) {
                    say_tool_used = true;
                    /* Extract spoken text for browser display */
                    cJSON *inp = cJSON_Parse(resp.calls[ci].input);
                    if (inp) {
                        cJSON *t = cJSON_GetObjectItemCaseSensitive(inp, "text");
                        if (cJSON_IsString(t) && t->valuestring && t->valuestring[0])
                            final_text = strdup(t->valuestring);
                        cJSON_Delete(inp);
                    }
                    break;
                }
            }

            /* Watchdog phase: track tool execution */
            if (resp.call_count == 1) {
                char tb[48];
                snprintf(tb, sizeof(tb), "tool:%.42s", resp.calls[0].name);
                agent_set_phase(tb);
            } else {
                agent_set_phase("tools:parallel");
            }
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);

            if (capture_image_called) {
                /* Trigger post-capture fade (exponential decay white→off) before resuming */
                led_indicator_set(LED_FLASH_FADE);
            }
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);

            /* say tool already completed the action — don't call LLM again */
            if (say_tool_used) {
                ESP_LOGI(TAG, "say tool used — skipping further LLM iterations");
                ws_server_broadcast_monitor("tool", "say tool used — skipping final TTS");
                break;
            }

            iteration++;
        }

        cJSON_Delete(messages);

        /* Always clear per-request overrides after the loop */
        llm_clear_request_override();
        llm_set_voice_max_tokens(0);

        if (!final_text && iteration >= LANG_AGENT_MAX_TOOL_ITER) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "agent: max tool iterations (%d) reached", LANG_AGENT_MAX_TOOL_ITER);
            ESP_LOGW(TAG, "%s", emsg);
            ws_server_broadcast_monitor("error", emsg);
        }

        /* 5. Send response */
        ws_server_broadcast_monitor("done", msg.chat_id);

        if (final_text && final_text[0]) {
            /* Auto-email long responses (>600 chars) from the websocket channel.
             * When emailed, TTS speaks a short local-LLM summary instead of
             * the full response.  Telegram already delivers to the user's
             * phone so no need to duplicate there.
             *
             * Skip auto-email if Apfel handled the turn — Apfel responses are
             * conversational by design (3B model, 4K context, no tools), never
             * essays. Emailing a 264-char Apfel refusal is pure noise. */
            size_t resp_len = strlen(final_text);
            bool auto_emailed = false;
            char *tts_summary = NULL;   /* short spoken version when emailed */

            if (resp_len > 600 && !using_apfel && strcmp(msg.channel, "websocket") == 0) {
                /* Build subject from first ~60 chars of response */
                char auto_subj[72];
                snprintf(auto_subj, sizeof(auto_subj), "Lango: %.60s%s",
                         final_text, resp_len > 60 ? "..." : "");
                for (char *p = auto_subj; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

                cJSON *email_in = cJSON_CreateObject();
                if (email_in) {
                    cJSON_AddStringToObject(email_in, "subject", auto_subj);
                    cJSON_AddStringToObject(email_in, "body",    final_text);
                    char *email_json = cJSON_PrintUnformatted(email_in);
                    cJSON_Delete(email_in);
                    if (email_json) {
                        char email_out[128];
                        esp_err_t mail_err = tool_smtp_execute(email_json, email_out, sizeof(email_out));
                        if (mail_err == ESP_OK) {
                            auto_emailed = true;
                            ESP_LOGI(TAG, "Auto-emailed long response (%u chars)", (unsigned)resp_len);
                            ws_server_broadcast_monitor("agent", "Long response auto-emailed");

                            /* Generate a 1-sentence spoken summary via local LLM.
                             * Truncate input to ~800 chars to keep the call fast. */
                            char *snippet = ps_calloc(1, 900);
                            if (snippet) {
                                snprintf(snippet, 900, "%.800s", final_text);
                                cJSON *sum_msgs = cJSON_CreateArray();
                                cJSON *sum_msg  = cJSON_CreateObject();
                                char *sum_prompt = NULL;
                                if (sum_msgs && sum_msg) {
                                    cJSON_AddStringToObject(sum_msg, "role", "user");
                                    /* Add sum_msg to array immediately so cJSON_Delete(sum_msgs) frees both */
                                    cJSON_AddItemToArray(sum_msgs, sum_msg);
                                    sum_msg = NULL;  /* owned by sum_msgs now */
                                    sum_prompt = ps_calloc(1, 1024);
                                }
                                if (sum_prompt) {
                                    snprintf(sum_prompt, 1024,
                                        "Summarise this in ONE short spoken sentence (under 30 words, "
                                        "no markdown, no emoji). End with: the full version has been emailed.\n\n%s",
                                        snippet);
                                    cJSON *msg_item = cJSON_GetArrayItem(sum_msgs, 0);
                                    cJSON_AddStringToObject(msg_item, "content", sum_prompt);

                                    agent_set_phase("llm:summariser");
                                    llm_set_request_override(llm_get_local_provider(), llm_get_local_text_model());
                                    llm_response_t sum_resp;
                                    memset(&sum_resp, 0, sizeof(sum_resp));
                                    esp_err_t sum_err = llm_chat_tools(
                                        "You create one-sentence spoken summaries.",
                                        sum_msgs, NULL, false, &sum_resp);

                                    if (sum_err == ESP_OK && sum_resp.text && sum_resp.text[0]) {
                                        tts_summary = sum_resp.text;
                                        sum_resp.text = NULL;
                                        ESP_LOGI(TAG, "TTS summary: %s", tts_summary);
                                    }
                                    llm_response_free(&sum_resp);
                                    free(sum_prompt);
                                }
                                cJSON_Delete(sum_msg);   /* NULL if transferred to array */
                                cJSON_Delete(sum_msgs);
                                free(snippet);
                                /* Restore original provider for next turn */
                                llm_clear_request_override();
                            }
                        } else {
                            ESP_LOGW(TAG, "Auto-email failed: %s", email_out);
                        }
                        free(email_json);
                    }
                }
            }

            /* Save to session */
            esp_err_t save_user = session_append(msg.chat_id, "user", msg.content);
            esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
            if (save_user != ESP_OK || save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for chat %s", msg.chat_id);
            }

            session_trim(msg.chat_id, LANG_SESSION_MAX_MSGS);

            if (is_telegram) {
                /* Telegram: edit the placeholder or send a new message */
                if (tg_placeholder_id > 0) {
                    telegram_edit_message(msg.chat_id, tg_placeholder_id, final_text);
                } else {
                    lang_msg_t out = {0};
                    strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                    strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                    out.content = final_text;
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop telegram response");
                        free(final_text);
                    } else {
                        final_text = NULL;
                    }
                }
            } else if (say_tool_used) {
                /* say tool already played audio through the speaker —
                 * send text to browser for display but skip TTS generation. */
                ESP_LOGI(TAG, "say tool already spoke — sending text-only to browser");
                ws_server_send_with_tts(msg.chat_id, final_text, NULL, NULL);
            } else if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0) {
                /* System/heartbeat: skip TTS entirely — just deliver text.
                 * TTS + I2S playback causes brownout resets from amp current. */
                ESP_LOGI(TAG, "System channel — skipping TTS, sending text only");
                lang_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = final_text;
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    free(final_text);
                } else {
                    final_text = NULL;
                }
            } else if (is_voice) {
                /* Voice interaction (wake word / PTT): generate TTS + play on speaker */
                led_indicator_set(LED_SPEAKING);
                ws_server_send_status(msg.chat_id, "tts_generating");
                char tts_id[9] = {0};
                #define TTS_MAX_CHARS 1500
                char tts_buf[TTS_MAX_CHARS + 1];
                const char *tts_text = (auto_emailed && tts_summary) ? tts_summary : final_text;
                if (strlen(tts_text) > TTS_MAX_CHARS) {
                    strncpy(tts_buf, tts_text, TTS_MAX_CHARS);
                    tts_buf[TTS_MAX_CHARS] = '\0';
                    int cut = TTS_MAX_CHARS - 1;
                    while (cut > 40) {
                        char c = tts_buf[cut];
                        if (c == '.' || c == '!' || c == '?') { cut++; break; }
                        cut--;
                    }
                    tts_buf[cut] = '\0';
                    tts_text = tts_buf;
                }
                /* Strip emoji / non-ASCII symbols that TTS engines can't pronounce */
                {
                    char *cleaned = tts_buf;
                    if (tts_text != tts_buf) {
                        strncpy(tts_buf, tts_text, TTS_MAX_CHARS);
                        tts_buf[TTS_MAX_CHARS] = '\0';
                    }
                    char *dst = cleaned;
                    const char *src = cleaned;
                    while (*src) {
                        unsigned char c = (unsigned char)*src;
                        if (c >= 0xF0) {
                            int skip = 4;
                            while (skip > 1 && src[1] && ((unsigned char)src[1] & 0xC0) == 0x80) { src++; skip--; }
                            src++;
                        } else if (c == 0xE2 && src[1] && src[2] && (unsigned char)src[1] >= 0x98) {
                            src += 3;
                        } else {
                            *dst++ = *src++;
                        }
                    }
                    *dst = '\0';
                    tts_text = cleaned;
                }
                oled_display_set_message(tts_text ? tts_text : final_text);

                /* Send text to browser immediately */
                if (strcmp(msg.channel, "websocket") == 0) {
                    ws_server_send_with_tts(msg.chat_id, final_text, NULL, NULL);
                }

                agent_set_phase("tts");

                /* First-sentence TTS split: if the response has a clean sentence
                 * boundary in [40, 220] chars with ≥20 chars remaining, generate
                 * the first sentence alone and start playback immediately, then
                 * generate the remainder while segment A is already playing.
                 * Savings ≈ duration of segment B's TTS call (~300-800ms).
                 * Gate on I2S playback path — WS/Telegram-only voice would need
                 * two tts_id events which the browser UI doesn't handle yet. */
                size_t split_at = 0;
#if LANG_I2S_AUDIO_ENABLED
                split_at = find_first_sentence_end(tts_text, 40, 220, 20);
#endif
                esp_err_t tts_err = ESP_FAIL;
                char tts_id_b[9] = {0};

                if (split_at > 0) {
                    /* Two-segment path */
                    char seg_a[240];
                    size_t a_len = (split_at < sizeof(seg_a)) ? split_at
                                                               : sizeof(seg_a) - 1;
                    memcpy(seg_a, tts_text, a_len);
                    seg_a[a_len] = '\0';
                    const char *seg_b = tts_text + split_at;
                    while (*seg_b == ' ' || *seg_b == '\n' || *seg_b == '\t') seg_b++;

                    ESP_LOGI(TAG, "TTS split: A=%u chars, B=%u chars",
                             (unsigned)a_len, (unsigned)strlen(seg_b));

                    tts_err = tts_generate(seg_a, tts_id);

                    /* Kick off segment A playback immediately (async, on queue) */
                    if (tts_err == ESP_OK && tts_id[0]) {
#if LANG_I2S_AUDIO_ENABLED
                        const uint8_t *wav_a = NULL;
                        size_t wav_a_len = 0;
                        if (tts_cache_get(tts_id, &wav_a, &wav_a_len) == ESP_OK) {
                            ESP_LOGI(TAG, "Playing TTS seg A via I2S (%u bytes, async)",
                                     (unsigned)wav_a_len);
                            i2s_audio_play_wav_async(wav_a, wav_a_len);
                        }
#endif
                        /* Generate segment B while A is already playing */
                        esp_err_t tts_err_b = tts_generate(seg_b, tts_id_b);
                        if (tts_err_b == ESP_OK && tts_id_b[0]) {
#if LANG_I2S_AUDIO_ENABLED
                            const uint8_t *wav_b = NULL;
                            size_t wav_b_len = 0;
                            if (tts_cache_get(tts_id_b, &wav_b, &wav_b_len) == ESP_OK) {
                                ESP_LOGI(TAG, "Enqueue TTS seg B (%u bytes)",
                                         (unsigned)wav_b_len);
                                i2s_audio_enqueue_wav(wav_b, wav_b_len);
                            }
#endif
                        } else {
                            ESP_LOGW(TAG, "TTS seg B failed: %s — seg A still plays",
                                     esp_err_to_name(tts_err_b));
                        }
                    }
                } else {
                    /* Single-shot path (response too short or no clean boundary) */
                    tts_err = tts_generate(tts_text, tts_id);

                    if (tts_err == ESP_OK && tts_id[0]) {
#if LANG_I2S_AUDIO_ENABLED
                        const uint8_t *wav_buf = NULL;
                        size_t wav_len = 0;
                        if (tts_cache_get(tts_id, &wav_buf, &wav_len) == ESP_OK) {
                            ESP_LOGI(TAG, "Playing TTS via I2S speaker (%u bytes, async)",
                                     (unsigned)wav_len);
                            i2s_audio_play_wav_async(wav_buf, wav_len);
                        }
#endif
                    }
                }

                if (tts_err == ESP_OK && tts_id[0]) {
                    if (strcmp(msg.channel, "websocket") == 0) {
                        /* Browser gets segment-A's tts_id for the play button.
                         * Segment B plays on-device via I2S queue; browser doesn't
                         * hear it unless they're also listening through the device. */
                        ws_server_send_with_tts(msg.chat_id, NULL, tts_id, NULL);
                    } else {
                        lang_msg_t out = {0};
                        strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                        strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                        out.content = final_text;
                        if (message_bus_push_outbound(&out) != ESP_OK) {
                            free(final_text);
                        } else {
                            final_text = NULL;
                        }
                    }
                } else if (strcmp(msg.channel, "websocket") != 0) {
                    /* Non-WS voice, TTS failed — send via outbound queue (Telegram etc.) */
                    lang_msg_t out = {0};
                    strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                    strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                    out.content = final_text;
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop final response");
                        free(final_text);
                    } else {
                        final_text = NULL;
                    }
                }
            } else {
                /* Text WebSocket / Telegram: no TTS, just deliver text */
                oled_display_set_message(final_text);

                const char *img_url = NULL;
                if (capture_image_called) {
                    struct stat img_st;
                    if (stat(LANG_CAMERA_CAPTURE_PATH, &img_st) == 0 && img_st.st_size > 0) {
                        img_url = "/camera/latest.jpg";
                    }
                }

                if (strcmp(msg.channel, "websocket") == 0) {
                    ESP_LOGI(TAG, "Text channel — sending text only (no TTS)");
                    ws_server_send_with_tts(msg.chat_id, final_text, NULL, img_url);
                } else {
                    lang_msg_t out = {0};
                    strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                    strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                    out.content = final_text;
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        free(final_text);
                    } else {
                        final_text = NULL;
                    }
                }
            }

            free(final_text);
            free(tts_summary);
            led_indicator_set(LED_READY);
            ws_server_send_status(msg.chat_id, "idle");

        } else {
            free(final_text);
            led_indicator_set(LED_ERROR);
            ws_server_broadcast_monitor("error", "agent: LLM returned empty response");
            char err_buf[128];
            const char *err_text;
            if (oom_restart) {
                err_text = "Memory exhausted — restarting...";
            } else if (last_err != ESP_OK) {
                snprintf(err_buf, sizeof(err_buf), "Error: %s", esp_err_to_name(last_err));
                err_text = err_buf;
            } else {
                err_text = "Sorry, I encountered an error.";
            }

            if (is_telegram && tg_placeholder_id > 0) {
                telegram_edit_message(msg.chat_id, tg_placeholder_id, err_text);
            } else {
                lang_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = strdup(err_text);
                if (out.content) {
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        free(out.content);
                    }
                }
            }

            /* Voice error: speak the error aloud so PTT/wake-word users
             * hear feedback instead of silence (the text-only dispatch
             * above goes to the browser but nothing comes out of the
             * speaker without TTS). */
            if (is_voice && !oom_restart) {
                char err_tts_id[9] = {0};
                if (tts_generate(err_text, err_tts_id) == ESP_OK && err_tts_id[0]) {
#if LANG_I2S_AUDIO_ENABLED
                    const uint8_t *wav = NULL;
                    size_t wav_len = 0;
                    if (tts_cache_get(err_tts_id, &wav, &wav_len) == ESP_OK) {
                        ESP_LOGI(TAG, "Speaking error via I2S (%u bytes)", (unsigned)wav_len);
                        i2s_audio_play_wav_async(wav, wav_len);
                    }
#endif
                }
            }

            /* Fallback push notification for system task failures.
             * If a heartbeat/cron task fails (e.g. morning briefing), the user
             * has no way to know unless we push a notification. */
            if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0) {
                char notify_json[384];
                snprintf(notify_json, sizeof(notify_json),
                    "{\"message\":\"Task '%.30s' failed: %.80s\","
                    "\"title\":\"Lango Task Failure\",\"priority\":\"high\","
                    "\"tags\":\"warning\"}",
                    msg.chat_id, err_text);
                char notify_out[128];
                tool_notify_execute(notify_json, notify_out, sizeof(notify_out));
                ESP_LOGW(TAG, "System task failure notified via push: %s", notify_out);
            }

            ws_server_send_status(msg.chat_id, "idle");
        }

        free(msg.content);

        if (oom_restart) {
            ws_server_broadcast_monitor("system", "OOM restart in 3s...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }

        atomic_store(&s_agent_busy, false);
        atomic_store(&s_turn_start_us, (int64_t)0);
        agent_set_phase("");

        ESP_LOGI(TAG, "PSRAM free: %d bytes  SRAM free: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    /* Pin to Core 1 (AI pipeline core).  Stack in PSRAM — safe with
     * SPIRAM_FETCH_INSTRUCTIONS=y + SPIRAM_RODATA=y (ESP-IDF 6.0 ESP32-S3).
     * Frees ~28KB of precious SRAM for WiFi/DNS/HTTP allocations. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        agent_loop_task, "agent_loop",
        LANG_AGENT_STACK, NULL,
        LANG_AGENT_PRIO, NULL,
        LANG_AGENT_CORE,
        MALLOC_CAP_SPIRAM);

    if (ret == pdPASS) {
        ESP_LOGI(TAG, "agent_loop task created on Core %d (stack=%u bytes)",
                 LANG_AGENT_CORE, (unsigned)LANG_AGENT_STACK);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "agent_loop task create failed (free_internal=%u, largest=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_FAIL;
}

bool agent_loop_is_busy(void) { return atomic_load(&s_agent_busy); }
