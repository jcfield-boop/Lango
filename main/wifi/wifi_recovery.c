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

/* Two trigger paths:
 *  (a) Consecutive: N fails in a row (no success in between) → fast
 *      path for outright wifi-stuck-association cases.
 *  (b) Sliding window: ≥M fails in last W attempts → catches
 *      flaky-but-not-dead patterns like 2026-05-01 17:06-17:39 where
 *      8 telegram polls failed across 33 min interleaved with
 *      successes (consecutive counter reset every 1-2 attempts and
 *      never crossed the threshold; user observed message delays).
 * Either path signals the worker. The window length must be small
 * enough to actually accumulate (telegram polls ~every 30s under
 * load, so a 10-attempt window spans ~5 min wall clock). */
#ifndef LANG_WIFI_RECOVERY_THRESHOLD
#define LANG_WIFI_RECOVERY_THRESHOLD 5
#endif
#ifndef LANG_WIFI_RECOVERY_WINDOW_SIZE
#define LANG_WIFI_RECOVERY_WINDOW_SIZE  10  /* must be ≤ 16 — packed in low bits of s_ring */
#endif
#ifndef LANG_WIFI_RECOVERY_WINDOW_FAILS
#define LANG_WIFI_RECOVERY_WINDOW_FAILS 5   /* ≥5 fails in last 10 → trigger */
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

/* Sliding-window ring buffer of the last WINDOW_SIZE outcomes, packed:
 *   bits  0-15: ring (bit i = 1 if attempt i was a failure)
 *   bits 16-19: write index (0..WINDOW_SIZE-1)
 * Atomic CAS update so multiple subsystems can record concurrently. */
static _Atomic uint32_t s_ring = 0;
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
            atomic_store(&s_ring, 0);
            atomic_store(&s_in_progress, false);
            continue;
        }

        atomic_store(&s_in_progress, true);
        atomic_store(&s_last_recovery_us, now);

        uint32_t r = atomic_load(&s_ring);
        int win_fails = __builtin_popcount(r & ((1u << LANG_WIFI_RECOVERY_WINDOW_SIZE) - 1));
        ESP_LOGW(TAG, "*** triggering wifi reconnect (consec=%d, window=%d/%d)",
                 atomic_load(&s_consecutive_failures),
                 win_fails, LANG_WIFI_RECOVERY_WINDOW_SIZE);

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

        /* Reset counters regardless of disconnect/connect return codes —
         * the wifi_manager's STA_DISCONNECTED handler will keep retrying
         * if e2 didn't take. We just want to stop counting failures
         * stacked up before this point. */
        atomic_store(&s_consecutive_failures, 0);
        atomic_store(&s_ring, 0);

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
    ESP_LOGI(TAG, "armed (consec=%d, window=%d/%d, cooldown=%ds)",
             LANG_WIFI_RECOVERY_THRESHOLD,
             LANG_WIFI_RECOVERY_WINDOW_FAILS, LANG_WIFI_RECOVERY_WINDOW_SIZE,
             LANG_WIFI_RECOVERY_COOLDOWN_MS / 1000);
    return ESP_OK;
}

/* Push one bit (1=fail, 0=success) into the sliding-window ring and
 * return the count of failures currently in the ring. Atomic via CAS
 * loop so concurrent record_* calls from telegram/llm/etc. compose. */
static int ring_record(bool failure)
{
    uint32_t old, new_val;
    do {
        old = atomic_load(&s_ring);
        uint32_t idx  = (old >> 16) & 0xF;
        uint32_t bits = old & ((1u << LANG_WIFI_RECOVERY_WINDOW_SIZE) - 1);
        bits &= ~(1u << idx);                /* clear slot we're overwriting */
        if (failure) bits |= (1u << idx);    /* set if this attempt failed */
        idx = (idx + 1) % LANG_WIFI_RECOVERY_WINDOW_SIZE;
        new_val = (idx << 16) | bits;
    } while (!atomic_compare_exchange_weak(&s_ring, &old, new_val));
    return __builtin_popcount(new_val & ((1u << LANG_WIFI_RECOVERY_WINDOW_SIZE) - 1));
}

void wifi_recovery_record_failure(const char *who)
{
    if (!s_initialised) return;
    if (atomic_load(&s_in_progress)) return;

    int n = atomic_fetch_add(&s_consecutive_failures, 1) + 1;
    int win = ring_record(true);
    ESP_LOGD(TAG, "fail from %s → consec=%d/%d window=%d/%d",
             who ? who : "?", n, LANG_WIFI_RECOVERY_THRESHOLD,
             win, LANG_WIFI_RECOVERY_WINDOW_FAILS);

    /* Path (a): consecutive failures (fast path on outright stuck wifi). */
    if (n >= LANG_WIFI_RECOVERY_THRESHOLD) {
        ESP_LOGW(TAG, "consec threshold reached (%d in a row, last from %s) — signalling worker",
                 n, who ? who : "?");
        xSemaphoreGive(s_trigger_sem);
        return;
    }

    /* Path (b): sliding-window failure rate. Catches interleaved
     * fail/success patterns that don't trip the consecutive counter
     * but still indicate a degraded link (the 2026-05-01 17:06-17:39
     * Telegram cluster). */
    if (win >= LANG_WIFI_RECOVERY_WINDOW_FAILS) {
        ESP_LOGW(TAG, "window threshold reached (%d/%d fails in last %d, last from %s) — signalling worker",
                 win, LANG_WIFI_RECOVERY_WINDOW_FAILS,
                 LANG_WIFI_RECOVERY_WINDOW_SIZE, who ? who : "?");
        xSemaphoreGive(s_trigger_sem);
    }
}

void wifi_recovery_record_success(const char *who)
{
    (void)who;
    if (!s_initialised) return;
    ring_record(false);  /* push 0 into the ring — a success doesn't
                          * by itself wipe the window, only ages out
                          * old failures by displacing them. */
    /* Cheap consec-reset: only do the atomic store if non-zero, so the hot
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
