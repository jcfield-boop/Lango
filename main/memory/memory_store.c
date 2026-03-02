#include "memory_store.h"
#include "langoustine_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "memory";

#define MEMORY_MAX_BYTES   LANG_MEMORY_MAX_BYTES
#define MEMORY_TRIM_BYTES  (LANG_MEMORY_MAX_BYTES - (1 * 1024))
#define MEMORY_DAILY_MAX_DAYS  7

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

/* Delete daily note files older than MEMORY_DAILY_MAX_DAYS.
 * LittleFS has real directories so opendir(LANG_LFS_MEMORY_DIR) works. */
static void purge_old_daily_notes(void)
{
    time_t cutoff = time(NULL) - (MEMORY_DAILY_MAX_DAYS * 86400);
    struct tm cutoff_tm;
    gmtime_r(&cutoff, &cutoff_tm);

    DIR *dir = opendir(LANG_LFS_MEMORY_DIR);
    if (!dir) return;

    int removed = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *fname = ent->d_name;
        /* Expect YYYY-MM-DD.md */
        if (strlen(fname) != 13 || strcmp(fname + 10, ".md") != 0) continue;

        int y = 0, m = 0, d = 0;
        if (sscanf(fname, "%4d-%2d-%2d", &y, &m, &d) != 3) continue;

        int file_val = y * 10000 + m * 100 + d;
        int cut_val  = (cutoff_tm.tm_year + 1900) * 10000
                     + (cutoff_tm.tm_mon + 1) * 100
                     + cutoff_tm.tm_mday;
        if (file_val >= cut_val) continue;

        char path[128];
        snprintf(path, sizeof(path), "%s/%s", LANG_LFS_MEMORY_DIR, fname);
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "Purged old daily note: %s", fname);
            removed++;
        }
    }
    closedir(dir);
    if (removed) ESP_LOGI(TAG, "Purged %d old daily note(s)", removed);
}

esp_err_t memory_store_init(void)
{
    ESP_LOGI(TAG, "Memory store initialized at %s", LANG_LFS_MEMORY_DIR);
    purge_old_daily_notes();
    return ESP_OK;
}

esp_err_t memory_read_long_term(char *buf, size_t size)
{
    FILE *f = fopen(LANG_MEMORY_FILE, "r");
    if (!f) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return ESP_OK;
}

esp_err_t memory_write_long_term(const char *content)
{
    size_t len = strlen(content);

    const char *write_ptr = content;
    size_t write_len = len;
    if (len > MEMORY_MAX_BYTES) {
        const char *tail = content + len - MEMORY_TRIM_BYTES;
        const char *nl = strchr(tail, '\n');
        if (nl && nl + 1 < content + len) tail = nl + 1;
        write_ptr = tail;
        write_len = strlen(tail);
        ESP_LOGW(TAG, "MEMORY.md trimmed: %d → %d bytes", (int)len, (int)write_len);
    }

    FILE *f = fopen(LANG_MEMORY_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", LANG_MEMORY_FILE);
        return ESP_FAIL;
    }
    fwrite(write_ptr, 1, write_len, f);
    fclose(f);
    ESP_LOGI(TAG, "Long-term memory updated (%d bytes)", (int)write_len);
    return ESP_OK;
}

esp_err_t memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char path[80];
    snprintf(path, sizeof(path), "%s/%s.md", LANG_LFS_MEMORY_DIR, date_str);

    FILE *f = fopen(path, "a");
    if (!f) {
        f = fopen(path, "w");
        if (!f) {
            ESP_LOGE(TAG, "Cannot open %s", path);
            return ESP_FAIL;
        }
        fprintf(f, "# %s\n\n", date_str);
    }

    fprintf(f, "%s\n", note);
    fclose(f);
    return ESP_OK;
}

esp_err_t memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[80];
        snprintf(path, sizeof(path), "%s/%s.md", LANG_LFS_MEMORY_DIR, date_str);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (offset > 0 && offset < size - 4) {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }

        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        offset += n;
        buf[offset] = '\0';
        fclose(f);
    }

    return ESP_OK;
}
