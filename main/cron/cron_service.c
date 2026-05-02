#include "cron/cron_service.h"
#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "gateway/ws_server.h"
#include "memory/session_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "cron";

#define MAX_CRON_JOBS  LANG_CRON_MAX_JOBS

static cron_job_t s_jobs[MAX_CRON_JOBS];
static int s_job_count = 0;
static TaskHandle_t s_cron_task = NULL;

static esp_err_t cron_save_jobs(void);

/* Parse a comma-separated day-of-week list into a bitmask.
 *   "sun,mon,tue,wed,thu,fri,sat" or a subset → bits 0..6
 *   "daily" / "*" / NULL / ""                → 0 (any day)
 * Case-insensitive, accepts full names too. Unknown tokens are ignored. */
static uint8_t parse_dow(const char *s)
{
    if (!s || !s[0]) return 0;
    if (strcmp(s, "*") == 0 || strcmp(s, "daily") == 0) return 0;

    static const struct { const char *name; uint8_t bit; } tbl[] = {
        {"sun", 1 << 0}, {"sunday",    1 << 0},
        {"mon", 1 << 1}, {"monday",    1 << 1},
        {"tue", 1 << 2}, {"tuesday",   1 << 2},
        {"wed", 1 << 3}, {"wednesday", 1 << 3},
        {"thu", 1 << 4}, {"thursday",  1 << 4},
        {"fri", 1 << 5}, {"friday",    1 << 5},
        {"sat", 1 << 6}, {"saturday",  1 << 6},
    };

    uint8_t mask = 0;
    const char *p = s;
    while (*p) {
        /* Skip separators */
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        /* Extract token (up to next separator) */
        char tok[16];
        size_t tlen = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && tlen < sizeof(tok) - 1) {
            tok[tlen++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
            p++;
        }
        tok[tlen] = '\0';
        for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
            if (strcmp(tok, tbl[i].name) == 0) { mask |= tbl[i].bit; break; }
        }
    }
    return mask;
}

/* Render a bitmask back to a short canonical string (e.g. "mon,wed").
 * Returns empty string when mask is 0 (any day). Used by cron_save_jobs. */
static void dow_mask_to_str(uint8_t mask, char *out, size_t out_size)
{
    static const char *names[7] = { "sun","mon","tue","wed","thu","fri","sat" };
    if (out_size == 0) return;
    out[0] = '\0';
    if (mask == 0) return;
    size_t off = 0;
    for (int i = 0; i < 7; i++) {
        if (!(mask & (1 << i))) continue;
        const char *sep = (off == 0) ? "" : ",";
        int n = snprintf(out + off, out_size - off, "%s%s", sep, names[i]);
        if (n <= 0 || (size_t)n >= out_size - off) break;
        off += (size_t)n;
    }
}

static bool cron_sanitize_destination(cron_job_t *job)
{
    bool changed = false;
    if (!job) {
        return false;
    }

    if (job->channel[0] == '\0') {
        strncpy(job->channel, LANG_CHAN_SYSTEM, sizeof(job->channel) - 1);
        changed = true;
    }

    if (strcmp(job->channel, LANG_CHAN_TELEGRAM) == 0) {
        if (job->chat_id[0] == '\0' || strcmp(job->chat_id, "cron") == 0) {
            ESP_LOGW(TAG, "Cron job %s has invalid telegram chat_id, fallback to system:cron",
                     job->id[0] ? job->id : "<new>");
            strncpy(job->channel, LANG_CHAN_SYSTEM, sizeof(job->channel) - 1);
            strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
            changed = true;
        }
    } else if (job->chat_id[0] == '\0') {
        strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
        changed = true;
    }

    return changed;
}

/* ── Persistence ──────────────────────────────────────────────── */

static void cron_generate_id(char *id_buf)
{
    uint32_t r = esp_random();
    snprintf(id_buf, 9, "%08x", (unsigned int)r);
}

static esp_err_t cron_load_jobs(void)
{
    FILE *f = fopen(LANG_CRON_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No cron file found, starting fresh");
        s_job_count = 0;
        return ESP_OK;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        ESP_LOGW(TAG, "Cron file invalid size: %ld", fsize);
        fclose(f);
        s_job_count = 0;
        return ESP_OK;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, fsize, f);
    buf[n] = '\0';
    fclose(f);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "Failed to parse cron JSON");
        s_job_count = 0;
        return ESP_OK;
    }

    cJSON *jobs_arr = cJSON_GetObjectItem(root, "jobs");
    if (!jobs_arr || !cJSON_IsArray(jobs_arr)) {
        cJSON_Delete(root);
        s_job_count = 0;
        return ESP_OK;
    }

    s_job_count = 0;
    bool repaired = false;
    cJSON *item;
    cJSON_ArrayForEach(item, jobs_arr) {
        if (s_job_count >= MAX_CRON_JOBS) break;

        cron_job_t *job = &s_jobs[s_job_count];
        memset(job, 0, sizeof(cron_job_t));

        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *kind_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(item, "message"));
        const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(item, "channel"));
        const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "chat_id"));

        if (!id || !name || !kind_str || !message) continue;

        strncpy(job->id, id, sizeof(job->id) - 1);
        strncpy(job->name, name, sizeof(job->name) - 1);
        strncpy(job->message, message, sizeof(job->message) - 1);
        if (strlen(message) >= sizeof(job->message)) {
            ESP_LOGW(TAG, "Job %s message truncated (%d→%d bytes)",
                     id, (int)strlen(message), (int)(sizeof(job->message) - 1));
        }
        strncpy(job->channel, channel ? channel : LANG_CHAN_SYSTEM,
                sizeof(job->channel) - 1);
        strncpy(job->chat_id, chat_id ? chat_id : "cron",
                sizeof(job->chat_id) - 1);
        if (cron_sanitize_destination(job)) {
            repaired = true;
        }

        cJSON *enabled_j = cJSON_GetObjectItem(item, "enabled");
        job->enabled = enabled_j ? cJSON_IsTrue(enabled_j) : true;

        cJSON *delete_j = cJSON_GetObjectItem(item, "delete_after_run");
        job->delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : false;

        if (strcmp(kind_str, "every") == 0) {
            job->kind = CRON_KIND_EVERY;
            cJSON *interval = cJSON_GetObjectItem(item, "interval_s");
            job->interval_s = (interval && cJSON_IsNumber(interval))
                              ? (uint32_t)interval->valuedouble : 0;
            /* Optional day-of-week constraint. Accepts a string ("sat",
             * "mon,wed,fri") or an array of strings (["sat","sun"]). */
            cJSON *dow = cJSON_GetObjectItem(item, "dow");
            if (dow) {
                if (cJSON_IsString(dow)) {
                    job->dow_mask = parse_dow(dow->valuestring);
                } else if (cJSON_IsArray(dow)) {
                    char joined[64] = {0};
                    size_t off = 0;
                    cJSON *d;
                    cJSON_ArrayForEach(d, dow) {
                        if (!cJSON_IsString(d)) continue;
                        const char *sep = (off == 0) ? "" : ",";
                        int n = snprintf(joined + off, sizeof(joined) - off,
                                         "%s%s", sep, d->valuestring);
                        if (n <= 0 || (size_t)n >= sizeof(joined) - off) break;
                        off += (size_t)n;
                    }
                    job->dow_mask = parse_dow(joined);
                }
            }
        } else if (strcmp(kind_str, "at") == 0) {
            job->kind = CRON_KIND_AT;
            cJSON *at_epoch = cJSON_GetObjectItem(item, "at_epoch");
            job->at_epoch = (at_epoch && cJSON_IsNumber(at_epoch))
                            ? (int64_t)at_epoch->valuedouble : 0;
        } else {
            continue; /* Unknown kind, skip */
        }

        cJSON *last_run = cJSON_GetObjectItem(item, "last_run");
        job->last_run = (last_run && cJSON_IsNumber(last_run))
                        ? (int64_t)last_run->valuedouble : 0;

        cJSON *next_run = cJSON_GetObjectItem(item, "next_run");
        job->next_run = (next_run && cJSON_IsNumber(next_run))
                        ? (int64_t)next_run->valuedouble : 0;

        s_job_count++;
    }

    cJSON_Delete(root);
    if (repaired) {
        cron_save_jobs();
    }
    ESP_LOGI(TAG, "Loaded %d cron jobs", s_job_count);
    return ESP_OK;
}

static esp_err_t cron_save_jobs(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *jobs_arr = cJSON_CreateArray();

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t *job = &s_jobs[i];
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "id", job->id);
        cJSON_AddStringToObject(item, "name", job->name);
        cJSON_AddBoolToObject(item, "enabled", job->enabled);
        cJSON_AddStringToObject(item, "kind",
            job->kind == CRON_KIND_EVERY ? "every" : "at");

        if (job->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", job->interval_s);
            if (job->dow_mask) {
                char dow_buf[32];
                dow_mask_to_str(job->dow_mask, dow_buf, sizeof(dow_buf));
                if (dow_buf[0]) {
                    cJSON_AddStringToObject(item, "dow", dow_buf);
                }
            }
        } else {
            cJSON_AddNumberToObject(item, "at_epoch", (double)job->at_epoch);
        }

        cJSON_AddStringToObject(item, "message", job->message);
        cJSON_AddStringToObject(item, "channel", job->channel);
        cJSON_AddStringToObject(item, "chat_id", job->chat_id);
        cJSON_AddNumberToObject(item, "last_run", (double)job->last_run);
        cJSON_AddNumberToObject(item, "next_run", (double)job->next_run);
        cJSON_AddBoolToObject(item, "delete_after_run", job->delete_after_run);

        cJSON_AddItemToArray(jobs_arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", jobs_arr);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize cron jobs");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(LANG_CRON_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", LANG_CRON_FILE);
        free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Cron save incomplete: %d/%d bytes", (int)written, (int)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %d cron jobs to %s", s_job_count, LANG_CRON_FILE);
    return ESP_OK;
}

/* ── Due-job processing ───────────────────────────────────────── */

static void cron_process_due_jobs(void)
{
    time_t now = time(NULL);

    bool changed = false;
    int fired = 0;

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t *job = &s_jobs[i];
        if (!job->enabled) continue;
        if (job->next_run <= 0) continue;
        if (job->next_run > now) continue;

        /* Skip massively overdue recurring jobs — they're stale after reboot.
         * If overdue by >2× interval, advance to the next future slot and
         * skip this run.  Prevents burst-firing all jobs after a reflash. */
        if (job->kind == CRON_KIND_EVERY && job->interval_s > 0) {
            int64_t overdue = (int64_t)(now - job->next_run);
            if (overdue > 2 * job->interval_s) {
                int64_t next = job->next_run;
                while (next <= now) {
                    next += job->interval_s;
                }
                /* Honour day-of-week (same as the post-fire path) so a
                 * reboot-induced catch-up doesn't land on the wrong day. */
                if (job->dow_mask) {
                    int rolls = 0;
                    while (rolls < 7) {
                        time_t tnext = (time_t)next;
                        struct tm tm_next;
                        localtime_r(&tnext, &tm_next);
                        if (job->dow_mask & (1 << tm_next.tm_wday)) break;
                        next += 86400;
                        rolls++;
                    }
                }
                ESP_LOGW(TAG, "Cron job %s (%s) overdue by %llds (>2x interval) — skipping to next",
                         job->name, job->id, (long long)overdue);
                job->next_run = next;
                changed = true;
                continue;
            }
        }

        /* Limit to 1 job per check interval to prevent burst-firing
         * all overdue jobs at once (causes network/power stress on boot) */
        if (fired >= 1) {
            ESP_LOGI(TAG, "Cron job %s (%s) deferred to next interval", job->name, job->id);
            continue;
        }

        /* Job is due — fire it */
        ESP_LOGI(TAG, "Cron job firing: %s (%s)", job->name, job->id);

        char mon_msg[64];
        snprintf(mon_msg, sizeof(mon_msg), "[cron] %s: %.40s", job->id, job->message);
        ws_server_broadcast_monitor("task", mon_msg);

        /* Clear stale session so cron jobs start with clean context
         * (same pattern as heartbeat — prevents LLM referencing old errors) */
        session_clear(job->chat_id);

        /* Push message to inbound queue */
        lang_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, job->channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, job->chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(job->message);

        bool dispatched = false;
        if (msg.content) {
            esp_err_t err = message_bus_push_inbound(&msg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to push cron message: %s — leaving job to retry on next tick",
                         esp_err_to_name(err));
                free(msg.content);
            } else {
                dispatched = true;
            }
        } else {
            ESP_LOGW(TAG, "strdup OOM on cron message — leaving job to retry on next tick");
        }

        /* If the dispatch failed (queue full, OOM), DON'T advance last_run /
         * next_run. The job stays "due" so the next heartbeat tick retries.
         * Without this, a single transient queue full would silently drop
         * a daily briefing for the whole day. */
        if (!dispatched) {
            continue;
        }

        /* Update state */
        job->last_run = now;

        if (job->kind == CRON_KIND_AT) {
            /* One-shot: disable or delete */
            if (job->delete_after_run) {
                /* Remove by shifting array */
                ESP_LOGI(TAG, "Deleting one-shot job: %s", job->name);
                for (int j = i; j < s_job_count - 1; j++) {
                    s_jobs[j] = s_jobs[j + 1];
                }
                s_job_count--;
                i--; /* Re-check this index */
            } else {
                job->enabled = false;
                job->next_run = 0;
            }
        } else {
            /* Recurring: advance from scheduled time, not wall clock,
             * to prevent drift when jobs fire late (e.g. after reboot).
             * A briefing scheduled for 06:05 stays at 06:05 even if
             * the device was down and fires the job at 08:30. */
            int64_t next = job->next_run + job->interval_s;
            while (next <= now) {
                next += job->interval_s;
            }

            /* Day-of-week constraint: roll forward by 1 day until the
             * scheduled weekday is in the allowed mask. Idempotent — a
             * skipped fire never drifts the job off its intended day. */
            if (job->dow_mask) {
                int rolls = 0;
                while (rolls < 7) {
                    time_t tnext = (time_t)next;
                    struct tm tm_next;
                    localtime_r(&tnext, &tm_next);
                    if (job->dow_mask & (1 << tm_next.tm_wday)) break;
                    next += 86400;
                    rolls++;
                }
                if (rolls > 0) {
                    ESP_LOGI(TAG, "cron %s: advanced next_run +%dd to match dow_mask=0x%02x",
                             job->id, rolls, job->dow_mask);
                }
            }
            job->next_run = next;
        }

        changed = true;
        fired++;
    }

    if (changed) {
        cron_save_jobs();
    }
}

static void cron_task_main(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LANG_CRON_CHECK_INTERVAL_MS));
        cron_process_due_jobs();
    }
}

/* ── Compute initial next_run for a new job ───────────────────── */

static void compute_initial_next_run(cron_job_t *job)
{
    time_t now = time(NULL);

    if (job->kind == CRON_KIND_EVERY) {
        job->next_run = now + job->interval_s;
    } else if (job->kind == CRON_KIND_AT) {
        if (job->at_epoch > now) {
            job->next_run = job->at_epoch;
        } else {
            /* Already in the past */
            job->next_run = 0;
            job->enabled = false;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t cron_service_init(void)
{
    return cron_load_jobs();
}

esp_err_t cron_service_start(void)
{
    if (s_cron_task) {
        ESP_LOGW(TAG, "Cron task already running");
        return ESP_OK;
    }

    /* Recompute next_run for all enabled jobs that don't have one */
    time_t now = time(NULL);
    for (int i = 0; i < s_job_count; i++) {
        cron_job_t *job = &s_jobs[i];
        if (job->enabled && job->next_run <= 0) {
            if (job->kind == CRON_KIND_EVERY) {
                job->next_run = now + job->interval_s;
            } else if (job->kind == CRON_KIND_AT && job->at_epoch > now) {
                job->next_run = job->at_epoch;
            }
        }
    }

    /* SRAM stack — required: cron_save_jobs() calls fopen/fwrite on LittleFS,
     * which briefly disables the flash cache.  PSRAM-stacked tasks hang when
     * the cache is disabled during a flash write (same rule as heartbeat/agent). */
    BaseType_t ok = xTaskCreatePinnedToCore(
        cron_task_main,
        "cron",
        4096,
        NULL,
        4,
        &s_cron_task,
        1   /* Core 1 (AI pipeline core) */
    );
    if (ok != pdPASS || !s_cron_task) {
        ESP_LOGE(TAG, "Failed to create cron task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Cron service started (%d jobs, check every %ds)",
             s_job_count, LANG_CRON_CHECK_INTERVAL_MS / 1000);
    return ESP_OK;
}

void cron_service_stop(void)
{
    if (s_cron_task) {
        vTaskDelete(s_cron_task);
        s_cron_task = NULL;
        ESP_LOGI(TAG, "Cron service stopped");
    }
}

esp_err_t cron_add_job(cron_job_t *job)
{
    if (s_job_count >= MAX_CRON_JOBS) {
        ESP_LOGW(TAG, "Max cron jobs reached (%d)", MAX_CRON_JOBS);
        return ESP_ERR_NO_MEM;
    }

    /* Generate ID */
    cron_generate_id(job->id);

    /* Validate/sanitize channel and chat_id before storing. */
    cron_sanitize_destination(job);

    /* Compute initial next_run */
    job->enabled = true;
    job->last_run = 0;
    compute_initial_next_run(job);

    /* Copy into static array */
    s_jobs[s_job_count] = *job;
    s_job_count++;

    cron_save_jobs();

    ESP_LOGI(TAG, "Added cron job: %s (%s) kind=%s next_run=%lld",
             job->name, job->id,
             job->kind == CRON_KIND_EVERY ? "every" : "at",
             (long long)job->next_run);

    char mon_msg[32];
    snprintf(mon_msg, sizeof(mon_msg), "[cron] added job %s", job->id);
    ws_server_broadcast_monitor("ws", mon_msg);
    return ESP_OK;
}

esp_err_t cron_remove_job(const char *job_id)
{
    for (int i = 0; i < s_job_count; i++) {
        if (strcmp(s_jobs[i].id, job_id) == 0) {
            ESP_LOGI(TAG, "Removing cron job: %s (%s)", s_jobs[i].name, job_id);

            /* Shift remaining jobs down */
            for (int j = i; j < s_job_count - 1; j++) {
                s_jobs[j] = s_jobs[j + 1];
            }
            s_job_count--;

            cron_save_jobs();

            char mon_msg[32];
            snprintf(mon_msg, sizeof(mon_msg), "[cron] removed job %s", job_id);
            ws_server_broadcast_monitor("ws", mon_msg);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Cron job not found: %s", job_id);
    return ESP_ERR_NOT_FOUND;
}

void cron_list_jobs(const cron_job_t **jobs, int *count)
{
    *jobs = s_jobs;
    *count = s_job_count;
}

esp_err_t cron_service_reload(void)
{
    /* Zero the count first: cron_task_main loops on s_job_count so it will
     * skip its next check harmlessly while we repopulate s_jobs[]. */
    s_job_count = 0;
    esp_err_t err = cron_load_jobs();
    ESP_LOGI(TAG, "Hot-reloaded %d cron jobs from disk", s_job_count);
    return err;
}
