/**
 * test_main.c — Unity test runner entry point for Langoustine test app.
 *
 * Flash and run:
 *   cd test_app
 *   idf.py set-target esp32s3
 *   idf.py build flash monitor
 *
 * All tests tagged [downsample], [cron], [ws_msg] will auto-run on boot.
 */

#include "unity.h"
#include "esp_log.h"

static const char *TAG = "test";

void app_main(void)
{
    ESP_LOGI(TAG, "=== Langoustine Unit Tests ===");
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    ESP_LOGI(TAG, "=== Tests complete — idle ===");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
