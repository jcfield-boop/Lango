#include "mcp/mcp_server.h"
#include "tools/tool_registry.h"
#include "memory/psram_alloc.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mcp";

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_TOOL_OUTPUT_SIZE 16384

/* ── JSON-RPC helpers ─────────────────────────────────────────── */

static void jsonrpc_error(cJSON *id, int code, const char *message,
                          char *output, size_t output_size)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);

    char *printed = cJSON_PrintUnformatted(resp);
    if (printed) {
        snprintf(output, output_size, "%s", printed);
        free(printed);
    }
    cJSON_Delete(resp);
}

static void jsonrpc_result(cJSON *id, cJSON *result,
                           char *output, size_t output_size)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON_AddItemToObject(resp, "result", result);

    char *printed = cJSON_PrintUnformatted(resp);
    if (printed) {
        snprintf(output, output_size, "%s", printed);
        free(printed);
    }
    cJSON_Delete(resp);
}

/* ── MCP method handlers ──────────────────────────────────────── */

static cJSON *handle_initialize(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "langoustine");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddItemToObject(result, "serverInfo", info);

    return result;
}

static cJSON *handle_tools_list(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools_arr = cJSON_CreateArray();

    /* Parse the cached tools JSON from the registry */
    const char *registry_json = tool_registry_get_tools_json();
    if (!registry_json) {
        cJSON_AddItemToObject(result, "tools", tools_arr);
        return result;
    }

    cJSON *src = cJSON_Parse(registry_json);
    if (!src || !cJSON_IsArray(src)) {
        cJSON_Delete(src);
        cJSON_AddItemToObject(result, "tools", tools_arr);
        return result;
    }

    /* Transform each tool: rename input_schema → inputSchema */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        cJSON *mcp_tool = cJSON_CreateObject();

        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name)) {
            cJSON_AddStringToObject(mcp_tool, "name", name->valuestring);
        }

        cJSON *desc = cJSON_GetObjectItem(item, "description");
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(mcp_tool, "description", desc->valuestring);
        }

        cJSON *schema = cJSON_GetObjectItem(item, "input_schema");
        if (schema) {
            cJSON_AddItemToObject(mcp_tool, "inputSchema",
                                  cJSON_Duplicate(schema, true));
        }

        cJSON_AddItemToArray(tools_arr, mcp_tool);
    }

    cJSON_Delete(src);
    cJSON_AddItemToObject(result, "tools", tools_arr);
    return result;
}

/* ── Tool execution in a background task ─────────────────────────
 *
 * BLOCKING HAZARD: tool_registry_execute() makes outbound HTTP calls
 * (web_search, get_weather, …) that can take 5–30 s. Running it directly
 * in the httpd task blocks ALL other HTTP requests for that duration, since
 * ESP-IDF's httpd is single-threaded. Instead, spawn a 16 KB task and wait
 * up to MCP_TOOL_TIMEOUT_MS for it to finish.
 *
 * Race-condition handling (timeout vs. late completion):
 *  - On success:  task signals semaphore, caller takes it, frees everything.
 *  - On timeout:  caller sets r->caller_gone under spinlock, then checks
 *                 r->task_done (set by task under same spinlock before signal).
 *                 Exactly one side frees resources — no double-free, no leak. */

#define MCP_TOOL_TIMEOUT_MS  10000   /* max ms httpd task waits for a tool */

typedef struct {
    char              name[64];      /* heap copy of tool name */
    char             *args;          /* heap, freed by task on any path */
    char             *output;        /* PSRAM, freed by the "winner" */
    size_t            output_sz;
    esp_err_t         result;
    bool              task_done;     /* set by task inside spinlock */
    bool              caller_gone;   /* set by caller inside spinlock */
    SemaphoreHandle_t done;
} mcp_run_t;

static portMUX_TYPE s_mcp_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void mcp_tool_task(void *arg)
{
    mcp_run_t *r = (mcp_run_t *)arg;
    r->result = tool_registry_execute(r->name, r->args ? r->args : "{}",
                                      r->output, r->output_sz);
    free(r->args);
    r->args = NULL;

    taskENTER_CRITICAL(&s_mcp_spinlock);
    r->task_done         = true;
    bool should_cleanup  = r->caller_gone;
    taskEXIT_CRITICAL(&s_mcp_spinlock);

    if (should_cleanup) {
        free(r->output);
        vSemaphoreDelete(r->done);
        free(r);
    } else {
        xSemaphoreGive(r->done);
    }
    vTaskDelete(NULL);
}

static cJSON *handle_tools_call(cJSON *params)
{
    cJSON *result       = cJSON_CreateObject();
    cJSON *content_arr  = cJSON_CreateArray();
    cJSON *content_item = cJSON_CreateObject();
    cJSON_AddStringToObject(content_item, "type", "text");

    const char *tool_name = NULL;
    cJSON *name_obj = cJSON_GetObjectItem(params, "name");
    if (name_obj && cJSON_IsString(name_obj)) {
        tool_name = name_obj->valuestring;
    }

    if (!tool_name) {
        cJSON_AddStringToObject(content_item, "text", "Missing tool name");
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        cJSON_AddBoolToObject(result, "isError", true);
        return result;
    }

    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    char *args_str = args ? cJSON_PrintUnformatted(args) : NULL;

    char *tool_output = ps_calloc(1, MCP_TOOL_OUTPUT_SIZE);
    if (!tool_output) {
        free(args_str);
        cJSON_AddStringToObject(content_item, "text", "Out of memory");
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        cJSON_AddBoolToObject(result, "isError", true);
        return result;
    }

    ESP_LOGI(TAG, "Executing tool: %s", tool_name);

    /* Run in background task to avoid blocking the single-threaded httpd. */
    mcp_run_t *run = calloc(1, sizeof(*run));
    SemaphoreHandle_t sem = run ? xSemaphoreCreateBinary() : NULL;

    if (!run || !sem) {
        if (sem) vSemaphoreDelete(sem);
        free(run);
        /* OOM fallback: run synchronously (blocks httpd briefly, but rare). */
        esp_err_t serr = tool_registry_execute(tool_name, args_str ? args_str : "{}",
                                               tool_output, MCP_TOOL_OUTPUT_SIZE);
        free(args_str);
        const char *text = (tool_output[0] != '\0') ? tool_output
                         : (serr == ESP_OK ? "" : "Tool execution failed");
        cJSON_AddStringToObject(content_item, "text", text);
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        if (serr != ESP_OK) cJSON_AddBoolToObject(result, "isError", true);
        free(tool_output);
        return result;
    }

    strncpy(run->name, tool_name, sizeof(run->name) - 1);
    run->args       = args_str;   /* ownership transferred */
    run->output     = tool_output; /* ownership transferred */
    run->output_sz  = MCP_TOOL_OUTPUT_SIZE;
    run->result     = ESP_FAIL;
    run->done       = sem;
    tool_output     = NULL;
    args_str        = NULL;

    BaseType_t ok = xTaskCreate(mcp_tool_task, "mcp_tool", 16 * 1024, run, 5, NULL);
    if (ok != pdPASS) {
        /* Task creation failed — run synchronously (blocks httpd briefly) */
        esp_err_t serr = tool_registry_execute(run->name, run->args ? run->args : "{}",
                                               run->output, run->output_sz);
        free(run->args);
        tool_output = run->output;
        vSemaphoreDelete(run->done);
        free(run);

        const char *text = (tool_output[0] != '\0') ? tool_output
                         : (serr == ESP_OK ? "" : "Tool execution failed");
        cJSON_AddStringToObject(content_item, "text", text);
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        if (serr != ESP_OK) cJSON_AddBoolToObject(result, "isError", true);
        free(tool_output);
        return result;
    }

    /* Wait for tool to finish (bounds httpd block to MCP_TOOL_TIMEOUT_MS). */
    bool completed = (xSemaphoreTake(run->done, pdMS_TO_TICKS(MCP_TOOL_TIMEOUT_MS)) == pdTRUE);

    if (completed) {
        /* Normal path: task finished, we own resources. */
        esp_err_t err = run->result;
        tool_output   = run->output;
        vSemaphoreDelete(run->done);
        free(run);

        const char *text = (tool_output[0] != '\0') ? tool_output
                         : (err == ESP_OK ? "" : "Tool execution failed");
        cJSON_AddStringToObject(content_item, "text", text);
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        if (err != ESP_OK) cJSON_AddBoolToObject(result, "isError", true);
        free(tool_output);
    } else {
        /* Timeout — set caller_gone under spinlock so task knows to clean up.
         * If the task finished in the race window between our semaphore timeout
         * and this critical section, task_done will be true and we must free. */
        taskENTER_CRITICAL(&s_mcp_spinlock);
        bool already_done = run->task_done;
        run->caller_gone  = true;
        taskEXIT_CRITICAL(&s_mcp_spinlock);

        if (already_done) {
            /* Task completed but we missed the semaphore — consume it, free. */
            xSemaphoreTake(run->done, 0);
            free(run->output);
            vSemaphoreDelete(run->done);
            free(run);
        }
        /* else: task still running, will see caller_gone=true and self-clean. */

        cJSON_AddStringToObject(content_item, "text",
                                "Tool timed out — use /api/message for long-running tools");
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        cJSON_AddBoolToObject(result, "isError", true);
    }

    return result;
}

static cJSON *handle_ping(void)
{
    return cJSON_CreateObject();
}

/* ── Main request dispatcher ──────────────────────────────────── */

esp_err_t mcp_handle_request(const char *body, size_t body_len,
                             char *output, size_t output_size)
{
    output[0] = '\0';

    cJSON *req = cJSON_ParseWithLength(body, body_len);
    if (!req) {
        jsonrpc_error(NULL, -32700, "Parse error", output, output_size);
        return ESP_OK;
    }

    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *method_obj = cJSON_GetObjectItem(req, "method");

    if (!method_obj || !cJSON_IsString(method_obj)) {
        jsonrpc_error(id, -32600, "Invalid Request", output, output_size);
        cJSON_Delete(req);
        return ESP_OK;
    }

    const char *method = method_obj->valuestring;
    cJSON *params = cJSON_GetObjectItem(req, "params");

    /* Notifications (no id) — accept silently */
    if (!id) {
        ESP_LOGD(TAG, "Notification: %s", method);
        cJSON_Delete(req);
        /* No response for notifications */
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MCP method: %s", method);

    cJSON *result = NULL;

    if (strcmp(method, "initialize") == 0) {
        result = handle_initialize();
    } else if (strcmp(method, "ping") == 0) {
        result = handle_ping();
    } else if (strcmp(method, "tools/list") == 0) {
        result = handle_tools_list();
    } else if (strcmp(method, "tools/call") == 0) {
        if (!params) {
            jsonrpc_error(id, -32602, "Invalid params", output, output_size);
            cJSON_Delete(req);
            return ESP_OK;
        }
        result = handle_tools_call(params);
    } else {
        jsonrpc_error(id, -32601, "Method not found", output, output_size);
        cJSON_Delete(req);
        return ESP_OK;
    }

    jsonrpc_result(id, result, output, output_size);
    cJSON_Delete(req);
    return ESP_OK;
}
