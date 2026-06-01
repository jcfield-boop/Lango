#include "tool_set_volume.h"
#include "audio/i2s_audio.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "tool_volume";

esp_err_t tool_set_volume_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *level_item = cJSON_GetObjectItem(root, "level");
    if (!level_item || !cJSON_IsNumber(level_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Missing or invalid 'level' (0-100)");
        return ESP_ERR_INVALID_ARG;
    }

    int pct = (int)level_item->valuedouble;
    cJSON_Delete(root);

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    uint8_t raw = (uint8_t)(pct * 255 / 100);
    i2s_audio_set_volume(raw);

    ESP_LOGI(TAG, "Volume → %d%% (raw %u)", pct, raw);
    snprintf(output, output_size, "Volume set to %d%%", pct);
    return ESP_OK;
}
