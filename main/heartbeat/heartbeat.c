/*
 * Heartbeat Service — periodic autonomous agent tasks
 *
 * Reads HEARTBEAT.md and dispatches due tasks to the agent.
 * Supports three task markers:
 *   [30m]          — runs every heartbeat cycle (default 30 min)
 *   [daily HH:MM]  — runs once per day at ~HH:MM (±20 min window)
 *   [ ]            — one-shot (agent marks [x] when done)
 *   [x]            — completed one-shot (skipped)
 *
 * Recurring tasks ([30m] and [daily]) never need file edits —
 * scheduling is tracked in RAM, saving flash wear.
 */
#include "heartbeat/heartbeat.h"
#include "langoustine_config.h"
#include "memory/session_mgr.h"
#include "bus/message_bus.h"
#include "gateway/ws_server.h"
#include "memory/psram_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"

static const char *TAG = "heartbeat";

/* ── Task types and parsing ─────────────────────────────────── */

#define MAX_HB_TASKS     12
#define HB_TEXT_LEN      384
#define HB_PROMPT_SIZE   2048

typedef enum {
    HB_INTERVAL,    /* [30m]  — every heartbeat cycle */
    HB_DAILY,       /* [daily HH:MM] — once per day at target time */
    HB_ONESHOT,     /* [ ] — one-shot pending */
    HB_DONE,        /* [x] — completed one-shot (skip) */
} hb_type_t;

typedef struct {
    hb_type_t type;
    uint8_t   hour;       /* daily tasks only */
    uint8_t   minute;     /* daily tasks only */
    char      text[HB_TEXT_LEN];
} hb_task_t;

/* ── Daily task dedup ────────────────────────────────────────── */

/* Track which daily tasks have fired today.
 * Index = order of daily tasks as parsed from file (0..MAX_DAILY-1). */
#define MAX_DAILY_TASKS  8
static int  s_daily_yday = -1;                /* tm_yday of last reset */
static bool s_daily_fired[MAX_DAILY_TASKS];   /* true = already fired today */

static void daily_reset_if_new_day(const struct tm *now)
{
    if (now->tm_yday != s_daily_yday) {
        memset(s_daily_fired, 0, sizeof(s_daily_fired));
        s_daily_yday = now->tm_yday;
        ESP_LOGI(TAG, "New day (yday=%d) — daily task flags reset", now->tm_yday);
    }
}

static bool daily_is_due(const hb_task_t *task, const struct tm *now, int daily_idx)
{
    if (daily_idx < 0 || daily_idx >= MAX_DAILY_TASKS) return false;
    if (s_daily_fired[daily_idx]) return false;

    int now_mins    = now->tm_hour * 60 + now->tm_min;
    int target_mins = task->hour * 60 + task->minute;
    int diff        = now_mins - target_mins;
    if (diff < 0) diff = -diff;

    /* Fire if within ±20 minute window of target time */
    return diff <= 20;
}

/* ── Parse HEARTBEAT.md ─────────────────────────────────────── */

static int parse_tasks(hb_task_t *tasks, int max)
{
    FILE *f = fopen(MIMI_HEARTBEAT_FILE, "r");
    if (!f) return 0;

    char line[512];
    int  count = 0;

    while (fgets(line, sizeof(line), f) && count < max) {
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        /* Must start with list marker: - or * */
        if (*p != '-' && *p != '*') continue;
        p++;
        if (*p != ' ') continue;
        p++;
        if (*p != '[') continue;
        p++;  /* past [ */

        hb_task_t *t = &tasks[count];
        memset(t, 0, sizeof(*t));

        if (p[0] == 'x' && p[1] == ']') {
            t->type = HB_DONE;
            p += 2;
        } else if (p[0] == ' ' && p[1] == ']') {
            t->type = HB_ONESHOT;
            p += 2;
        } else if (strncmp(p, "30m]", 4) == 0) {
            t->type = HB_INTERVAL;
            p += 4;
        } else if (strncmp(p, "daily ", 6) == 0) {
            p += 6;
            int h = 0, m = 0;
            if (sscanf(p, "%d:%d]", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                t->type   = HB_DAILY;
                t->hour   = (uint8_t)h;
                t->minute = (uint8_t)m;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
            } else {
                continue;  /* malformed daily spec */
            }
        } else {
            continue;  /* unknown marker */
        }

        /* Skip whitespace after ] */
        while (*p && (*p == ' ' || *p == '\t')) p++;

        /* Copy task text, trim trailing newline */
        strncpy(t->text, p, HB_TEXT_LEN - 1);
        t->text[HB_TEXT_LEN - 1] = '\0';
        size_t len = strlen(t->text);
        while (len > 0 && (t->text[len - 1] == '\n' || t->text[len - 1] == '\r'))
            t->text[--len] = '\0';

        if (t->text[0] != '\0') count++;
    }

    fclose(f);
    return count;
}

/* ── Build prompt and dispatch ───────────────────────────────── */

static TimerHandle_t s_heartbeat_timer = NULL;

static bool heartbeat_send(void)
{
    /* Allocate tasks from PSRAM — too large for timer task stack (8KB). */
    hb_task_t *tasks = (hb_task_t *)ps_calloc(MAX_HB_TASKS, sizeof(hb_task_t));
    if (!tasks) {
        ESP_LOGE(TAG, "Failed to alloc task array");
        return false;
    }

    int n = parse_tasks(tasks, MAX_HB_TASKS);
    if (n == 0) {
        free(tasks);
        ESP_LOGD(TAG, "No tasks in HEARTBEAT.md");
        return false;
    }

    /* Get current local time for daily scheduling */
    time_t now_t = time(NULL);
    struct tm now;
    localtime_r(&now_t, &now);
    daily_reset_if_new_day(&now);

    /* Collect due tasks into a targeted prompt */
    char *prompt = (char *)ps_calloc(1, HB_PROMPT_SIZE);
    if (!prompt) {
        ESP_LOGE(TAG, "Failed to alloc heartbeat prompt");
        return false;
    }

    int  off        = 0;
    int  due_count  = 0;
    int  daily_idx  = 0;
    bool has_oneshots = false;

    off += snprintf(prompt + off, HB_PROMPT_SIZE - off,
                    "Heartbeat check (%02d:%02d). Execute these tasks:\n\n",
                    now.tm_hour, now.tm_min);

    for (int i = 0; i < n; i++) {
        hb_task_t *t = &tasks[i];
        bool due = false;

        switch (t->type) {
        case HB_INTERVAL:
            due = true;
            break;
        case HB_DAILY:
            due = daily_is_due(t, &now, daily_idx);
            if (due) {
                s_daily_fired[daily_idx] = true;
                ESP_LOGI(TAG, "Daily task %d due at %02d:%02d (target %02d:%02d)",
                         daily_idx, now.tm_hour, now.tm_min, t->hour, t->minute);
            }
            daily_idx++;
            break;
        case HB_ONESHOT:
            due = true;
            has_oneshots = true;
            break;
        case HB_DONE:
        default:
            break;
        }

        if (due && off < (HB_PROMPT_SIZE - 400)) {
            due_count++;
            off += snprintf(prompt + off, HB_PROMPT_SIZE - off,
                            "%d. %s\n", due_count, t->text);
        }
    }

    free(tasks);  /* done parsing — free before potential early return */

    if (due_count == 0) {
        free(prompt);
        ESP_LOGD(TAG, "No tasks due this cycle (%02d:%02d)", now.tm_hour, now.tm_min);
        return false;
    }

    if (has_oneshots) {
        off += snprintf(prompt + off, HB_PROMPT_SIZE - off,
                        "\nFor any [ ] one-shot tasks in %s, mark them [x] with "
                        "edit_file after completing.\n", MIMI_HEARTBEAT_FILE);
    }

    off += snprintf(prompt + off, HB_PROMPT_SIZE - off,
                    "\nKeep responses concise. Just do the tasks, no meta-commentary.");

    /* Clear stale session history so the agent doesn't reference old errors.
     * Each heartbeat cycle should start with a clean context. */
    session_clear("heartbeat");

    /* Push to agent */
    lang_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, LANG_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    msg.content = prompt;  /* bus takes ownership */

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to push heartbeat: %s", esp_err_to_name(err));
        free(prompt);
        return false;
    }

    ESP_LOGI(TAG, "Dispatched %d task(s) at %02d:%02d", due_count, now.tm_hour, now.tm_min);
    ws_server_broadcast_monitor("task", "[heartbeat] triggered");
    return true;
}

/* ── Timer callback ───────────────────────────────────────────── */

static void heartbeat_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    heartbeat_send();
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t heartbeat_init(void)
{
    ESP_LOGI(TAG, "Heartbeat service initialized (file: %s, interval: %ds)",
             MIMI_HEARTBEAT_FILE, MIMI_HEARTBEAT_INTERVAL_MS / 1000);
    return ESP_OK;
}

esp_err_t heartbeat_start(void)
{
    if (s_heartbeat_timer) {
        ESP_LOGW(TAG, "Heartbeat timer already running");
        return ESP_OK;
    }

    s_heartbeat_timer = xTimerCreate(
        "heartbeat",
        pdMS_TO_TICKS(MIMI_HEARTBEAT_INTERVAL_MS),
        pdTRUE,    /* auto-reload */
        NULL,
        heartbeat_timer_callback
    );

    if (!s_heartbeat_timer) {
        ESP_LOGE(TAG, "Failed to create heartbeat timer");
        return ESP_FAIL;
    }

    if (xTimerStart(s_heartbeat_timer, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Heartbeat started (every %d min)", MIMI_HEARTBEAT_INTERVAL_MS / 60000);
    return ESP_OK;
}

void heartbeat_stop(void)
{
    if (s_heartbeat_timer) {
        xTimerStop(s_heartbeat_timer, pdMS_TO_TICKS(1000));
        xTimerDelete(s_heartbeat_timer, pdMS_TO_TICKS(1000));
        s_heartbeat_timer = NULL;
        ESP_LOGI(TAG, "Heartbeat stopped");
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}
