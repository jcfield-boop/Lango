#include "wifi/wifi_recovery.h"
#include "langoustine_config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

static const char *TAG = "wifi_recovery";

/* Threshold: trigger a reconnect after this many consecutive outbound
 * HTTP failures with no successes in between. Five was chosen so a
 * single transient blip (1-2 fails) doesn't kick a reconnect, but a
 * genuine stuck association (5+ fails over a few minutes) does. */
#ifndef LANG_WIFI_RECOVERY_THRESHOLD
#define LANG_WIFI_RECOVERY_THRESHOLD 5
#endif

/* Cooldown: don't trigger another reconnect within this window after
 * a previous one. Stops a tight reconnect loop when the cause of
 * failures is upstream and the reconnect itself can't help. */
#ifndef LANG_WIFI_RECOVERY_COOLDOWN_MS
#define LANG_WIFI_RECOVERY_COOLDOWN_MS  (60 * 1000)
#endif

static _Atomic int  s_consecutive_failures = 0;
static _Atomic bool s_in_progress          = false;
static _Atomic int64_t s_last_recovery_us  = 0;
static SemaphoreHandle_t s_trigger_sem     = NULL;
static TaskHandle_t      s_worker          = NULL;
static bool              s_initialised     = false;

static void recovery_worker(void *arg)
{
    (void)arg;
    while (1) {
        /* Block until someone signals via the binary semaphore */
        if (xSemaphoreTake(s_trigger_sem, portMAX_DELAY) != pdTRUE) continue;

        /* Cooldown gate */
        int64_t now = esp_timer_get_time();
        int64_t last = atomic_load(&s_last_recovery_us);
        if (last && (now - last) < (int64_t)LANG_WIFI_RECOVERY_COOLDOWN_MS * 1000LL) {
            ESP_LOGI(TAG, "skip — within %ds cooldown of last reconnect",
                     LANG_WIFI_RECOVERY_COOLDOWN_MS / 1000);
            atomic_store(&s_consecutive_failures, 0);
            atomic_store(&s_in_progress, false);
            continue;
        }

        atomic_store(&s_in_progress, true);
        atomic_store(&s_last_recovery_us, now);

        ESP_LOGW(TAG, "*** triggering wifi reconnect after %d consecutive failures",
                 atomic_load(&s_consecutive_failures));

        /* Disassociate and reassociate. esp_wifi_disconnect tears down
         * the IP layer (DHCP lease released, DNS resolvers cleared);
         * esp_wifi_connect kicks the SDK to scan + reauth + reassoc.
         * The IP_EVENT_STA_GOT_IP handler in wifi_manager will fire
         * once a fresh lease lands.
         *
         * Sleep 500ms between disconnect and connect to ensure the AP
         * sees the disassoc cleanly before we re-knock. */
        esp_err_t e1 = esp_wifi_disconnect();
        ESP_LOGI(TAG, "esp_wifi_disconnect → %s", esp_err_to_name(e1));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_err_t e2 = esp_wifi_connect();
        ESP_LOGI(TAG, "esp_wifi_connect → %s", esp_err_to_name(e2));

        /* Reset counter regardless of disconnect/connect return codes —
         * the wifi_manager's STA_DISCONNECTED handler will keep retrying
         * if e2 didn't take. We just want to stop counting failures
         * stacked up before this point. */
        atomic_store(&s_consecutive_failures, 0);

        /* Hold the in-progress flag for ~3s after connect to give DHCP
         * + DNS time to settle before any caller fires another request.
         * Callers that check in_progress will skip incrementing during
         * this window. */
        vTaskDelay(pdMS_TO_TICKS(3000));
        atomic_store(&s_in_progress, false);

        ESP_LOGI(TAG, "recovery cycle complete");
    }
}

esp_err_t wifi_recovery_init(void)
{
    if (s_initialised) return ESP_OK;

    s_trigger_sem = xSemaphoreCreateBinary();
    if (!s_trigger_sem) {
        ESP_LOGE(TAG, "failed to create trigger semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* Worker stays in SRAM (touches wifi/lwIP), small stack (~3 KB
     * is plenty — the body just calls two SDK functions and logs). */
    BaseType_t rc = xTaskCreatePinnedToCore(
        recovery_worker, "wifi_recov", 3072, NULL,
        5, &s_worker, 0);  /* Core 0 (wifi/lwIP) */
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn worker: %d", (int)rc);
        vSemaphoreDelete(s_trigger_sem);
        s_trigger_sem = NULL;
        return ESP_FAIL;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "armed (threshold=%d, cooldown=%ds)",
             LANG_WIFI_RECOVERY_THRESHOLD, LANG_WIFI_RECOVERY_COOLDOWN_MS / 1000);
    return ESP_OK;
}

void wifi_recovery_record_failure(const char *who)
{
    if (!s_initialised) return;
    if (atomic_load(&s_in_progress)) return;

    int n = atomic_fetch_add(&s_consecutive_failures, 1) + 1;
    ESP_LOGD(TAG, "fail from %s → %d/%d",
             who ? who : "?", n, LANG_WIFI_RECOVERY_THRESHOLD);

    if (n >= LANG_WIFI_RECOVERY_THRESHOLD) {
        ESP_LOGW(TAG, "threshold reached (%d consecutive fails, last from %s) — signalling worker",
                 n, who ? who : "?");
        /* Signal — worker will pick it up. xSemaphoreGive from any
         * task context is safe; from ISR we'd need FromISR variant
         * but no caller is in ISR context. */
        xSemaphoreGive(s_trigger_sem);
    }
}

void wifi_recovery_record_success(const char *who)
{
    (void)who;
    if (!s_initialised) return;
    /* Cheap success: only do the atomic store if non-zero, so the hot
     * path (every successful HTTP) skips a write. */
    if (atomic_load(&s_consecutive_failures) > 0) {
        atomic_store(&s_consecutive_failures, 0);
    }
}

bool wifi_recovery_in_progress(void)
{
    return atomic_load(&s_in_progress);
}

int wifi_recovery_consecutive_failures(void)
{
    return atomic_load(&s_consecutive_failures);
}
