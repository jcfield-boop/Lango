#include "tool_memory_read.h"
#include "memory/memory_store.h"
#include "memory/psram_alloc.h"
#include "langoustine_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "tool_mem_read";

esp_err_t tool_memory_read_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    esp_err_t err = memory_read_long_term(output, output_size);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read MEMORY.md (%s)", esp_err_to_name(err));
        return err;
    }

    if (output[0] == '\0') {
        snprintf(output, output_size, "(MEMORY.md is empty)");
    }

    ESP_LOGI(TAG, "memory_read: %d bytes", (int)strlen(output));
    return ESP_OK;
}
