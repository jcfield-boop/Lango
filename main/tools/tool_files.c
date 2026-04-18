#include "tools/tool_files.h"
#include "langoustine_config.h"
#include "memory/psram_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_files";

#define MAX_FILE_SIZE (32 * 1024)

/* Paths that the LLM may NOT write or edit (prompt injection protection) */
static const char * const WRITE_DENYLIST[] = {
    "/lfs/config/SOUL.md",
    "/lfs/config/SERVICES.md",
    "/lfs/cron.json",
    NULL
};

/**
 * Trim a line-oriented file to its last `max_lines` lines. Only rewrites the
 * file when it actually exceeds the cap. Called after any append to soak.md
 * to enforce LANG_SOAK_MAX_LINES without relying on the LLM to remember.
 *
 * Counts newline bytes in a single streaming pass, then — only if over cap —
 * slurps the file into a PSRAM buffer and rewrites the tail.
 */
static void trim_file_to_last_lines(const char *path, int max_lines)
{
    if (!path || max_lines <= 0) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Pass 1: count lines + file size cheaply */
    int total_lines = 0;
    long file_size = 0;
    char chunk[256];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        file_size += (long)n;
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') total_lines++;
        }
    }

    if (total_lines <= max_lines || file_size <= 0 || file_size > MAX_FILE_SIZE) {
        fclose(f);
        return;  /* nothing to do */
    }

    /* Pass 2: slurp + rewrite the tail */
    rewind(f);
    char *buf = ps_malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGW(TAG, "trim %s: alloc failed", path);
        return;
    }
    size_t got = fread(buf, 1, (size_t)file_size, f);
    buf[got] = '\0';
    fclose(f);

    /* Walk from end backwards, counting newlines. We want to keep the last
     * `max_lines` lines — scan until we've passed (total_lines - max_lines)
     * newlines from the start, then the next byte is the start of the kept tail. */
    int skip = total_lines - max_lines;
    size_t cut = 0;
    int seen = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            seen++;
            if (seen == skip) {
                cut = i + 1;
                break;
            }
        }
    }

    FILE *out = fopen(path, "w");
    if (!out) {
        free(buf);
        ESP_LOGW(TAG, "trim %s: reopen failed", path);
        return;
    }
    size_t tail_len = got - cut;
    fwrite(buf + cut, 1, tail_len, out);
    fclose(out);
    free(buf);
    ESP_LOGI(TAG, "trimmed %s: %d→%d lines (%ld→%u bytes)",
             path, total_lines, max_lines, file_size, (unsigned)tail_len);
}

/**
 * Validate that a path starts with /lfs/ and contains no ".." traversal.
 */
static bool validate_path(const char *path)
{
    if (!path) return false;
    if (strncmp(path, "/lfs/", 5) != 0) return false;
    if (strstr(path, "..") != NULL) return false;
    return true;
}

/**
 * Returns true if path is in the write denylist.
 */
static bool path_is_write_denied(const char *path)
{
    for (int i = 0; WRITE_DENYLIST[i]; i++) {
        if (strcmp(path, WRITE_DENYLIST[i]) == 0) return true;
    }
    return false;
}

/* ── read_file ─────────────────────────────────────────────── */

esp_err_t tool_read_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must start with /lfs/ and must not contain '..'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    size_t max_read = output_size - 1;
    if (max_read > MAX_FILE_SIZE) max_read = MAX_FILE_SIZE;

    size_t n = fread(output, 1, max_read, f);
    output[n] = '\0';
    fclose(f);

    ESP_LOGI(TAG, "read_file: %s (%d bytes)", path, (int)n);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── write_file ────────────────────────────────────────────── */

esp_err_t tool_write_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must start with /lfs/ and must not contain '..'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_write_denied(path)) {
        snprintf(output, output_size, "Error: writes to %s are not permitted", path);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        snprintf(output, output_size, "Error: missing 'content' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *append_item = cJSON_GetObjectItem(root, "append");
    bool do_append = append_item && cJSON_IsBool(append_item) && cJSON_IsTrue(append_item);

    /* For append mode: if the existing file is non-empty and does not end
     * with '\n', prepend one so consecutive entries stay on separate lines.
     * This handles LLM-generated content that omits the trailing newline. */
    if (do_append) {
        FILE *peek = fopen(path, "r");
        if (peek) {
            bool needs_nl = false;
            if (fseek(peek, -1, SEEK_END) == 0) {
                char last = 0;
                if (fread(&last, 1, 1, peek) == 1) {
                    needs_nl = (last != '\n');
                }
            }
            fclose(peek);
            if (needs_nl) {
                FILE *nl_f = fopen(path, "a");
                if (nl_f) { fwrite("\n", 1, 1, nl_f); fclose(nl_f); }
            }
        }
    }

    FILE *f = fopen(path, do_append ? "a" : "w");
    if (!f) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if (written != len) {
        snprintf(output, output_size, "Error: wrote %d of %d bytes to %s", (int)written, (int)len, path);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Firmware-enforced cap for soak.md so it stays bounded regardless of
     * whether the nightly summary cron actually runs the truncate step. */
    if (do_append && strcmp(path, LANG_SOAK_FILE) == 0) {
        trim_file_to_last_lines(path, LANG_SOAK_MAX_LINES);
    }

    snprintf(output, output_size, "OK: %s %d bytes to %s",
             do_append ? "appended" : "wrote", (int)written, path);
    ESP_LOGI(TAG, "write_file: %s (%d bytes)", path, (int)written);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── edit_file ─────────────────────────────────────────────── */

esp_err_t tool_edit_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *old_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "old_string"));
    const char *new_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "new_string"));

    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must start with /lfs/ and must not contain '..'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (path_is_write_denied(path)) {
        snprintf(output, output_size, "Error: edits to %s are not permitted", path);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!old_str || !new_str) {
        snprintf(output, output_size, "Error: missing 'old_string' or 'new_string' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Read existing file */
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        snprintf(output, output_size, "Error: file too large or empty (%ld bytes)", file_size);
        fclose(f);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Allocate buffer for the result (old content + possible expansion) */
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t max_result = file_size + (new_len > old_len ? new_len - old_len : 0) + 1;
    char *buf = ps_malloc(file_size + 1);
    char *result = ps_malloc(max_result);
    if (!buf || !result) {
        free(buf);
        free(result);
        fclose(f);
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, file_size, f);
    buf[n] = '\0';
    fclose(f);

    /* Find and replace first occurrence */
    char *pos = strstr(buf, old_str);
    if (!pos) {
        snprintf(output, output_size, "Error: old_string not found in %s", path);
        free(buf);
        free(result);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    size_t prefix_len = pos - buf;
    memcpy(result, buf, prefix_len);
    memcpy(result + prefix_len, new_str, new_len);
    size_t suffix_start = prefix_len + old_len;
    size_t suffix_len = n - suffix_start;
    memcpy(result + prefix_len + new_len, buf + suffix_start, suffix_len);
    size_t total = prefix_len + new_len + suffix_len;
    result[total] = '\0';

    free(buf);

    /* Write back */
    f = fopen(path, "w");
    if (!f) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        free(result);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    fwrite(result, 1, total, f);
    fclose(f);
    free(result);

    snprintf(output, output_size, "OK: edited %s (replaced %d bytes with %d bytes)", path, (int)old_len, (int)new_len);
    ESP_LOGI(TAG, "edit_file: %s", path);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── list_dir ──────────────────────────────────────────────── */

/**
 * Recursive helper: walk dir_path and emit every file whose full path
 * starts with prefix (or all files if prefix is NULL). LittleFS uses
 * real directories, so a single opendir(LANG_LFS_BASE) only returns
 * top-level entries — we must recurse into subdirectories.
 */
static int list_dir_recursive(const char *dir_path, const char *prefix,
                               char *output, size_t output_size, size_t *off)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && *off < output_size - 1) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        if (ent->d_type == DT_DIR) {
            /* Recurse — LittleFS real directory */
            count += list_dir_recursive(full_path, prefix, output, output_size, off);
        } else {
            /* Regular file — apply prefix filter */
            if (prefix && strncmp(full_path, prefix, strlen(prefix)) != 0) {
                continue;
            }
            *off += snprintf(output + *off, output_size - *off, "%s\n", full_path);
            count++;
        }
    }
    closedir(dir);
    return count;
}

esp_err_t tool_list_dir_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    const char *prefix = NULL;
    if (root) {
        cJSON *pfx = cJSON_GetObjectItem(root, "prefix");
        if (pfx && cJSON_IsString(pfx)) {
            prefix = pfx->valuestring;
        }
    }

    size_t off = 0;
    int count = list_dir_recursive(LANG_LFS_BASE, prefix, output, output_size, &off);

    if (count == 0) {
        snprintf(output, output_size, "(no files found)");
    }

    ESP_LOGI(TAG, "list_dir: %d files (prefix=%s)", count, prefix ? prefix : "(none)");
    cJSON_Delete(root);
    return ESP_OK;
}
