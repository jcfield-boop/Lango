#include "ota_manager.h"
#include "langoustine_config.h"
#include "gateway/ws_server.h"
#include "led/led_indicator.h"
#include "agent/agent_loop.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

/* Minimum free SRAM required BEFORE creating the OTA task (checked in ota_start_async).
 * The OTA task stack itself is 14KB (OTA_TASK_STACK), so the effective headroom after
 * task creation is (OTA_MIN_FREE_HEAP - OTA_TASK_STACK) = 8KB for flash write buffers,
 * OTA handle, and other small SRAM allocations. TLS and HTTP buffers go to PSRAM. */
#define OTA_TASK_STACK       (14 * 1024)
#define OTA_MIN_FREE_HEAP    (22 * 1024)  /* 14KB stack + 8KB headroom */

/* HTTP receive buffer for OTA download. */
#define OTA_HTTP_BUF_SIZE    (16 * 1024)

/* How long to wait for agent to finish before aborting OTA. */
#define OTA_WAIT_TIMEOUT_MS  (30 * 1000)
#define OTA_WAIT_POLL_MS     500
#define OTA_WAIT_LOG_MS      5000

/* ── Status tracking ─────────────────────────────────────────── */

static portMUX_TYPE  s_status_mux  = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t  s_status      = { .state = OTA_STATE_IDLE };
static volatile bool s_ota_running = false;
static char          s_ota_url[256];

static void status_set(ota_state_t state, uint8_t pct,
                       const char *version, const char *err_msg)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.state        = state;
    s_status.progress_pct = pct;
    if (version) {
        strncpy(s_status.new_version, version, sizeof(s_status.new_version) - 1);
        s_status.new_version[sizeof(s_status.new_version) - 1] = '\0';
    }
    if (err_msg) {
        strncpy(s_status.error_msg, err_msg, sizeof(s_status.error_msg) - 1);
        s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    } else {
        s_status.error_msg[0] = '\0';
    }
    portEXIT_CRITICAL(&s_status_mux);
}

ota_status_t ota_get_status(void)
{
    ota_status_t snap;
    portENTER_CRITICAL(&s_status_mux);
    snap = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return snap;
}

/* ── Core update logic (blocking) ─────────────────────────────── */

esp_err_t ota_update_from_url(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    /* Step 1: Wait for agent to finish current turn */
    status_set(OTA_STATE_PENDING, 0, NULL, NULL);
    ws_server_broadcast_monitor("ota", "OTA pending: waiting for agent to finish...");

    int waited_ms = 0, log_accum = 0;
    while (agent_loop_is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(OTA_WAIT_POLL_MS));
        waited_ms  += OTA_WAIT_POLL_MS;
        log_accum  += OTA_WAIT_POLL_MS;
        if (log_accum >= OTA_WAIT_LOG_MS) {
            char msg[80];
            snprintf(msg, sizeof(msg),
                     "OTA pending: waiting for agent... (%ds / 30s)",
                     waited_ms / 1000);
            ws_server_broadcast_monitor("ota", msg);
            log_accum = 0;
        }
        if (waited_ms >= OTA_WAIT_TIMEOUT_MS) {
            const char *abort_msg = "OTA aborted: agent did not finish in 30s";
            ESP_LOGW(TAG, "%s", abort_msg);
            ws_server_broadcast_monitor("ota", abort_msg);
            status_set(OTA_STATE_ERROR, 0, NULL, abort_msg);
            return ESP_ERR_TIMEOUT;
        }
    }

    /* Step 3: Begin download */
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "OTA start: %.90s", url);
        ESP_LOGI(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
    }
    led_indicator_set(LED_OTA);
    status_set(OTA_STATE_DOWNLOADING, 0, NULL, NULL);

    esp_http_client_config_t http_cfg = {
        .url                   = url,
        .timeout_ms            = 120000,
        .buffer_size           = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx        = 512,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .max_redirection_count = 3,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config         = &http_cfg,
        .http_client_init_cb = NULL,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA begin failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        status_set(OTA_STATE_ERROR, 0, NULL, msg);
        led_indicator_set(LED_ERROR);
        return ret;
    }

    /* Step 4: Validate image header before writing any bytes */
    esp_app_desc_t new_desc;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_desc);
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA image invalid (bad header): %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        esp_https_ota_abort(ota_handle);
        status_set(OTA_STATE_ERROR, 0, NULL, msg);
        led_indicator_set(LED_ERROR);
        return ret;
    }
    {
        const esp_app_desc_t *running = esp_app_get_description();
        char msg[128];
        snprintf(msg, sizeof(msg), "OTA image OK: v%s -> v%s",
                 running->version, new_desc.version);
        ESP_LOGI(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        status_set(OTA_STATE_DOWNLOADING, 0, new_desc.version, NULL);
    }

    /* Step 5: Stream and write with per-decile progress broadcasts */
    int img_size = esp_https_ota_get_image_size(ota_handle);
    int last_pct = -1;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        if (img_size > 0) {
            int read = esp_https_ota_get_image_len_read(ota_handle);
            int pct  = (int)((int64_t)read * 100 / img_size);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            int bucket = (pct / 10) * 10;
            if (bucket > (last_pct / 10) * 10 && bucket > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "OTA progress: %d%% (%d / %d bytes)", pct, read, img_size);
                ws_server_broadcast_monitor("ota", msg);
                last_pct = pct;
            }
            portENTER_CRITICAL(&s_status_mux);
            s_status.progress_pct = (uint8_t)pct;
            portEXIT_CRITICAL(&s_status_mux);
        }
    }
    if (ret != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA write failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        esp_https_ota_abort(ota_handle);
        status_set(OTA_STATE_ERROR, 0, NULL, msg);
        led_indicator_set(LED_ERROR);
        return ret;
    }

    /* Step 6: Verify, commit, reboot */
    status_set(OTA_STATE_VERIFYING, 100, NULL, NULL);
    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        status_set(OTA_STATE_REBOOTING, 100, NULL, NULL);
        ws_server_broadcast_monitor("ota", "OTA complete - rebooting in 3s...");
        ESP_LOGI(TAG, "OTA successful, restarting");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "OTA finish failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        status_set(OTA_STATE_ERROR, 0, NULL, msg);
        led_indicator_set(LED_ERROR);
    }

    return ret;
}

/* ── Async task wrapper ──────────────────────────────────────────── */

static void ota_task(void *arg)
{
    (void)arg;
    esp_err_t ret = ota_update_from_url(s_ota_url);
    if (ret != ESP_OK) {
        /* Hold ERROR state for 10s so clients can read it before it clears */
        vTaskDelay(pdMS_TO_TICKS(10000));
        status_set(OTA_STATE_IDLE, 0, NULL, NULL);
    }
    s_ota_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_start_async(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (s_ota_running) {
        ws_server_broadcast_monitor("ota", "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    /* Heap guard: checked HERE before xTaskCreate so the 14KB task stack hasn't been
     * allocated yet. Checking inside the task body is too late — the stack consumes
     * OTA_TASK_STACK bytes before the guard can run, giving a false "too low" reading. */
    uint32_t free_sram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_sram < OTA_MIN_FREE_HEAP) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "OTA aborted: SRAM too low (%lu B, need %d B)",
                 (unsigned long)free_sram, OTA_MIN_FREE_HEAP);
        ESP_LOGW(TAG, "%s", msg);
        ws_server_broadcast_monitor("ota", msg);
        status_set(OTA_STATE_ERROR, 0, NULL, msg);
        vTaskDelay(pdMS_TO_TICKS(5000));
        status_set(OTA_STATE_IDLE, 0, NULL, NULL);
        return ESP_ERR_NO_MEM;
    }

    /* Clear any previous error state before launching */
    status_set(OTA_STATE_PENDING, 0, NULL, NULL);
    s_ota_running = true;
    BaseType_t ret = xTaskCreate(ota_task, "ota_update", OTA_TASK_STACK, NULL, 5, NULL);
    if (ret != pdPASS) {
        s_ota_running = false;
        status_set(OTA_STATE_IDLE, 0, NULL, NULL);
        ws_server_broadcast_monitor("ota", "OTA task create failed (OOM)");
        return ESP_ERR_NO_MEM;
    }

    ws_server_broadcast_monitor("ota", "OTA task started");
    return ESP_OK;
}
