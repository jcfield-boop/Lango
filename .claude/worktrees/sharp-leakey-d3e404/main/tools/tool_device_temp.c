#include "tool_device_temp.h"

#include <stdio.h>
#include "esp_log.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "tool_temp";

/* Cache sensor handle — avoid 50ms install/uninstall overhead per call */
static temperature_sensor_handle_t s_temp_handle = NULL;

esp_err_t tool_device_temp_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    if (!s_temp_handle) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_temp_handle) != ESP_OK) {
            snprintf(output, output_size, "Error: temperature sensor init failed");
            return ESP_FAIL;
        }
        if (temperature_sensor_enable(s_temp_handle) != ESP_OK) {
            temperature_sensor_uninstall(s_temp_handle);
            s_temp_handle = NULL;
            snprintf(output, output_size, "Error: temperature sensor enable failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Temperature sensor initialized");
    }

    float celsius = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(s_temp_handle, &celsius);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: temperature read failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Chip temperature: %.1f°C", celsius);
    snprintf(output, output_size, "Device temperature: %.1f°C (ESP32-S3 internal sensor)", celsius);
    return ESP_OK;
}

esp_err_t device_temp_get_celsius(float *out_celsius)
{
    if (!out_celsius) return ESP_ERR_INVALID_ARG;

    if (!s_temp_handle) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_temp_handle) != ESP_OK ||
            temperature_sensor_enable(s_temp_handle) != ESP_OK) {
            s_temp_handle = NULL;
            return ESP_FAIL;
        }
    }
    return temperature_sensor_get_celsius(s_temp_handle, out_celsius);
}
