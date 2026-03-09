#include "tool_device_restart.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tool_restart";

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));  /* 1.5s — enough for response to be sent */
    ESP_LOGI(TAG, "Restarting device now");
    esp_restart();
}

esp_err_t tool_device_restart_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    snprintf(output, output_size,
             "{\"ok\":true,\"message\":\"Restarting in ~1.5 seconds...\"}");

    /* Spawn a one-shot task so the response is sent before the reboot */
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "device_restart scheduled");
    return ESP_OK;
}
