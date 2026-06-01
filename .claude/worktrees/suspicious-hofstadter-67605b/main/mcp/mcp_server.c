#include "mcp/mcp_server.h"
#include "tools/tool_registry.h"
#include "memory/psram_alloc.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

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

static cJSON *handle_tools_call(cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content_arr = cJSON_CreateArray();
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

    /* Serialize arguments back to JSON string for tool_registry_execute */
    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    char *args_str = NULL;
    if (args) {
        args_str = cJSON_PrintUnformatted(args);
    }

    /* Allocate tool output buffer from PSRAM */
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
    esp_err_t err = tool_registry_execute(tool_name,
                                          args_str ? args_str : "{}",
                                          tool_output, MCP_TOOL_OUTPUT_SIZE);
    free(args_str);

    if (err == ESP_OK) {
        cJSON_AddStringToObject(content_item, "text", tool_output);
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
    } else {
        const char *err_text = (tool_output[0] != '\0') ? tool_output : "Tool execution failed";
        cJSON_AddStringToObject(content_item, "text", err_text);
        cJSON_AddItemToArray(content_arr, content_item);
        cJSON_AddItemToObject(result, "content", content_arr);
        cJSON_AddBoolToObject(result, "isError", true);
    }

    free(tool_output);
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
