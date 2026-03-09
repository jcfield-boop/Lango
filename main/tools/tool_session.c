#include "tool_session.h"
#include "langoustine_config.h"

#include <stdio.h>
#include "esp_err.h"
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_session";

esp_err_t tool_session_clear_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id"));
    if (!chat_id || chat_id[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'chat_id' is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate chat_id: no path traversal or special chars */
    for (const char *p = chat_id; *p; p++) {
        if (*p == '/' || *p == '.' || *p == '\0') {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: invalid chat_id");
            return ESP_ERR_INVALID_ARG;
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/ws_%s.jsonl", LANG_LFS_SESSION_DIR, chat_id);

    cJSON_Delete(root);

    if (unlink(path) != 0) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"reason\":\"Session file not found: %s\"}", path);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "session_clear: deleted %s", path);
    snprintf(output, output_size,
             "{\"ok\":true,\"message\":\"Session history cleared for chat_id '%s'\"}",
             chat_id);
    return ESP_OK;
}
