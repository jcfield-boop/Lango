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
#include "audio/tts_client.h"
#include "audio/i2s_audio.h"
#include "telegram/telegram_bot.h"
#include "led/led_indicator.h"

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

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) return;

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) return;

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const mimi_msg_t *msg)
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

static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
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
            if (!ctxs[i].output) { all_alloc = false; break; }
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
                    ESP_LOGW(TAG, "tool_par[%d] timed out after 25s — proceeding with empty result", i);
                    ws_server_broadcast_monitor("error", "tool_par timeout (25s) — partial results");
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
        }
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
        free(patched_input);

        int tool_out_len = (int)strlen(tool_output);
        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, tool_out_len);

        if (strncmp(tool_output, "Error:", 6) == 0) {
            char emsg[160];
            snprintf(emsg, sizeof(emsg), "[%s] %.100s", call->name, tool_output);
            ws_server_broadcast_monitor("error", emsg);
        } else {
            char preview[128];
            snprintf(preview, sizeof(preview), "[%s] %d bytes: %.40s%s",
                     call->name, tool_out_len, tool_output,
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
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        atomic_store(&s_agent_busy, true);
        int64_t turn_start_us = esp_timer_get_time();
        memory_tool_reset_turn();
        web_search_reset_turn();

        /* SRAM guard: if heap is critically low, restart cleanly rather than
         * letting the LLM call hit malloc(NULL) and cause a hard panic.
         * Threshold: mbedTLS (~15KB) + HTTP bufs (~8KB) + safety margin = 26KB.
         * A hard restart shows as ESP_RST_SW (intentional), not ESP_RST_PANIC. */
        {
            uint32_t sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            ESP_LOGI(TAG, "Turn start: SRAM free=%lu B", (unsigned long)sram_free);
            if (sram_free < 22 * 1024) {
                char wmsg[80];
                snprintf(wmsg, sizeof(wmsg),
                         "SRAM critically low (%lu B) — restarting to recover heap",
                         (unsigned long)sram_free);
                ESP_LOGW(TAG, "%s", wmsg);
                ws_server_broadcast_monitor("system", wmsg);
                vTaskDelay(pdMS_TO_TICKS(1000));  /* brief pause for log flush */
                esp_restart();
            } else if (sram_free < 28 * 1024) {
                char wmsg[72];
                snprintf(wmsg, sizeof(wmsg), "SRAM low at turn start: %lu B — risk of OOM",
                         (unsigned long)sram_free);
                ESP_LOGW(TAG, "%s", wmsg);
                ws_server_broadcast_monitor("system", wmsg);
            }
        }

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        {
            char mon[64];
            snprintf(mon, sizeof(mon), "from %s:%s", msg.channel, msg.chat_id);
            ws_server_broadcast_monitor("task", mon);
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
        bool is_telegram = (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0);
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

        while (iteration < LANG_AGENT_MAX_TOOL_ITER) {
            /* Soft per-turn timeout: 180s. Daily briefing heartbeats need 4-6 LLM
             * iterations with web_search + RSS + send_email — each round-trip to
             * OpenRouter takes 5-15s, easily totaling 2-3 minutes. */
            if ((esp_timer_get_time() - turn_start_us) > 180000000LL) {
                ESP_LOGW(TAG, "Agent soft timeout at iter %d — abandoning turn", iteration);
                ws_server_broadcast_monitor("error", "agent turn timeout");
                final_text = strdup("[Sorry, that took too long. Please retry.]");
                break;
            }

#if LANG_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, LANG_CHAN_SYSTEM) != 0 && !is_telegram) {
                mimi_msg_t status = {0};
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
            llm_response_t resp;
            bool force_this_iter = (force_memory_tool && iteration == 0);
            err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                           force_this_iter,
                                           ws_stream_progress, &stream_ctx,
                                           &resp);

            if (err == ESP_ERR_HTTP_CONNECT && !retry_done) {
                retry_done = true;
                ws_server_broadcast_monitor("error", "LLM: connection failed, retrying in 2s...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                err = llm_chat_tools_streaming(system_prompt, messages, tools_json,
                                               force_this_iter,
                                               ws_stream_progress, &stream_ctx,
                                               &resp);
            }

            if (err != ESP_OK) {
                char emsg[80];
                snprintf(emsg, sizeof(emsg), "LLM call failed: %s", esp_err_to_name(err));
                ESP_LOGE(TAG, "%s", emsg);
                ws_server_broadcast_monitor("error", emsg);
                if (err == ESP_ERR_NO_MEM) { oom_restart = true; }
                break;
            }

            if (!resp.tool_use) {
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    llm_response_free(&resp);
                    if (!final_text) {
                        ESP_LOGE(TAG, "strdup OOM for final_text (%d bytes)", (int)resp.text_len);
                        oom_restart = true;
                    }
                    break;
                }
                bool do_recovery = resp.truncated && !recovery_tried;
                llm_response_free(&resp);
                if (do_recovery) {
                    recovery_tried = true;
                    iteration++;
                    ws_server_broadcast_monitor("error", "LLM: truncated, injecting recovery");
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

            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Track if capture_image was invoked this turn; flash LED white for illumination */
            for (int ci = 0; ci < resp.call_count; ci++) {
                if (strcmp(resp.calls[ci].name, "capture_image") == 0) {
                    capture_image_called = true;
                    led_indicator_set(LED_CAPTURING);
                    break;
                }
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
            iteration++;
        }

        cJSON_Delete(messages);

        if (!final_text && iteration >= LANG_AGENT_MAX_TOOL_ITER) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "agent: max tool iterations (%d) reached", LANG_AGENT_MAX_TOOL_ITER);
            ESP_LOGW(TAG, "%s", emsg);
            ws_server_broadcast_monitor("error", emsg);
        }

        /* 5. Send response */
        ws_server_broadcast_monitor("done", msg.chat_id);

        if (final_text && final_text[0]) {
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
                    mimi_msg_t out = {0};
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
            } else if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0) {
                /* System/heartbeat: skip TTS entirely — just deliver text.
                 * TTS + I2S playback causes brownout resets from amp current. */
                ESP_LOGI(TAG, "System channel — skipping TTS, sending text only");
                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = final_text;
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    free(final_text);
                } else {
                    final_text = NULL;
                }
            } else {
                /* WebSocket: generate TTS and send */
                led_indicator_set(LED_SPEAKING);
                ws_server_send_status(msg.chat_id, "tts_generating");
                char tts_id[9] = {0};
                /* Limit TTS input to keep WAV download manageable for browsers.
                 * Truncate at the last sentence boundary (. ! ?) within the limit.
                 * Full text is still sent to the browser for display. */
                #define TTS_MAX_CHARS 500
                char tts_buf[TTS_MAX_CHARS + 1];
                const char *tts_text = final_text;
                if (strlen(final_text) > TTS_MAX_CHARS) {
                    strncpy(tts_buf, final_text, TTS_MAX_CHARS);
                    tts_buf[TTS_MAX_CHARS] = '\0';
                    /* Walk back to last sentence-ending punctuation */
                    int cut = TTS_MAX_CHARS - 1;
                    while (cut > 40) {
                        char c = tts_buf[cut];
                        if (c == '.' || c == '!' || c == '?') { cut++; break; }
                        cut--;
                    }
                    tts_buf[cut] = '\0';
                    tts_text = tts_buf;
                }
                /* Strip emoji / non-ASCII symbols that TTS engines can't pronounce.
                 * UTF-8 4-byte sequences (0xF0...) cover U+10000+ (emoji, symbols).
                 * Also strip common 3-byte dingbats (U+2600-U+27BF, U+2B50, etc). */
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
                            /* Skip 4-byte UTF-8 sequence (emoji) */
                            int skip = 4;
                            while (skip > 1 && src[1] && ((unsigned char)src[1] & 0xC0) == 0x80) { src++; skip--; }
                            src++;
                        } else if (c == 0xE2 && src[1] && src[2] && (unsigned char)src[1] >= 0x98) {
                            /* Skip 3-byte dingbats/symbols (U+2600+) */
                            src += 3;
                        } else {
                            *dst++ = *src++;
                        }
                    }
                    *dst = '\0';
                    tts_text = cleaned;
                }
                esp_err_t tts_err = tts_generate(tts_text, tts_id);

                /* Only include image URL if a capture actually succeeded (file exists) */
                const char *img_url = NULL;
                if (capture_image_called) {
                    struct stat img_st;
                    if (stat(LANG_CAMERA_CAPTURE_PATH, &img_st) == 0 && img_st.st_size > 0) {
                        img_url = "/camera/latest.jpg";
                    }
                }
                if (tts_err == ESP_OK && tts_id[0]) {
                    /* Send message with tts_id so browser auto-plays */
                    ws_server_send_with_tts(msg.chat_id, final_text, tts_id, img_url);

#if LANG_I2S_AUDIO_ENABLED
                    /* Play TTS audio through local MAX98357A speaker.
                     * Skip for system/heartbeat — amp current causes brownout resets. */
                    if (strcmp(msg.channel, LANG_CHAN_SYSTEM) != 0) {
                        const uint8_t *wav_buf = NULL;
                        size_t wav_len = 0;
                        if (tts_cache_get(tts_id, &wav_buf, &wav_len) == ESP_OK) {
                            ESP_LOGI(TAG, "Playing TTS via I2S speaker (%u bytes, async)", (unsigned)wav_len);
                            i2s_audio_play_wav_async(wav_buf, wav_len);
                        }
                    } else {
                        ESP_LOGI(TAG, "Skipping I2S playback for system channel");
                    }
#endif
                } else if (img_url && strcmp(msg.channel, "websocket") == 0) {
                    /* No TTS but have an image — send directly so image_url is included */
                    ws_server_send_with_tts(msg.chat_id, final_text, NULL, img_url);
                } else {
                    /* No TTS, no image — send via outbound queue (handles Telegram etc.) */
                    mimi_msg_t out = {0};
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
            }

            free(final_text);
            led_indicator_set(LED_READY);
            ws_server_send_status(msg.chat_id, "idle");

        } else {
            free(final_text);
            led_indicator_set(LED_ERROR);
            ws_server_broadcast_monitor("error", "agent: LLM returned empty response");
            const char *err_text = oom_restart
                ? "Memory exhausted — restarting..."
                : "Sorry, I encountered an error.";

            if (is_telegram && tg_placeholder_id > 0) {
                telegram_edit_message(msg.chat_id, tg_placeholder_id, err_text);
            } else {
                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = strdup(err_text);
                if (out.content) {
                    if (message_bus_push_outbound(&out) != ESP_OK) {
                        free(out.content);
                    }
                }
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
    /* Pin to Core 1 (AI pipeline core); stack must be in SRAM (accesses LittleFS/NVS) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        agent_loop_task, "agent_loop",
        LANG_AGENT_STACK, NULL,
        LANG_AGENT_PRIO, NULL,
        LANG_AGENT_CORE);

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
