#include "session_mgr.h"
#include "langoustine_config.h"
#include "memory/psram_alloc.h"
#include "memory/memory_store.h"
#include "llm/llm_proxy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

/* Line buffer for reading JSONL — sized to match content budget */
#define SESS_LINE_BUF  (LANG_SESSION_HISTORY_MAX_BYTES)

/* ─── PSRAM hot cache for the active session ─── */
static char   s_cache_chat_id[32] = {0};
static cJSON *s_cache_arr         = NULL;  /* owned cJSON array in PSRAM */

static void cache_invalidate(void)
{
    if (s_cache_arr) {
        cJSON_Delete(s_cache_arr);
        s_cache_arr = NULL;
    }
    s_cache_chat_id[0] = '\0';
}

/* ─── Helpers ─── */

static void session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s/ws_%s.jsonl", LANG_LFS_SESSION_DIR, chat_id);
}

/* Load entire session from file into a cJSON array (all messages). */
static cJSON *load_session_from_file(const char *chat_id)
{
    char path[96];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return cJSON_CreateArray();

    cJSON *arr = cJSON_CreateArray();
    char *line = ps_malloc(SESS_LINE_BUF);
    if (!line) { fclose(f); return arr; }

    while (fgets(line, SESS_LINE_BUF, f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Keep only role + content (strip ts etc.) */
        cJSON *role    = cJSON_GetObjectItem(obj, "role");
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        if (role && content) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
            cJSON_AddItemToArray(arr, entry);
        }
        cJSON_Delete(obj);
    }

    free(line);
    fclose(f);
    return arr;
}

/* Ensure the cache is populated for the given chat_id. */
static void cache_ensure(const char *chat_id)
{
    if (s_cache_arr && strcmp(s_cache_chat_id, chat_id) == 0) {
        return;  /* already cached */
    }

    cache_invalidate();

    s_cache_arr = load_session_from_file(chat_id);
    strncpy(s_cache_chat_id, chat_id, sizeof(s_cache_chat_id) - 1);
    s_cache_chat_id[sizeof(s_cache_chat_id) - 1] = '\0';

    ESP_LOGI(TAG, "Session cache loaded for %s (%d messages)",
             chat_id, cJSON_GetArraySize(s_cache_arr));
}

/* Extract the last max_msgs entries from a cJSON array as a new deep-copied array. */
static cJSON *extract_tail(const cJSON *src_arr, int max_msgs)
{
    int total = cJSON_GetArraySize(src_arr);
    int start = (total > max_msgs) ? (total - max_msgs) : 0;

    cJSON *result = cJSON_CreateArray();
    for (int i = start; i < total; i++) {
        const cJSON *src = cJSON_GetArrayItem(src_arr, i);
        const cJSON *role    = cJSON_GetObjectItemCaseSensitive(src, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(src, "content");
        if (role && content) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
            cJSON_AddItemToArray(result, entry);
        }
    }
    return result;
}

/* ─── Public API ─── */

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s (max %d msgs, %dKB budget)",
             LANG_LFS_SESSION_DIR, LANG_SESSION_MAX_MSGS,
             LANG_SESSION_HISTORY_MAX_BYTES / 1024);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    /* 1. Write to JSONL file */
    char path[96];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }
    fclose(f);

    /* 2. Update hot cache if it's for the active session */
    if (s_cache_arr && strcmp(s_cache_chat_id, chat_id) == 0) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", role);
        cJSON_AddStringToObject(entry, "content", content);
        cJSON_AddItemToArray(s_cache_arr, entry);
    }

    return ESP_OK;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    cache_ensure(chat_id);

    cJSON *arr = extract_tail(s_cache_arr, max_msgs);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

cJSON *session_get_history_cjson(const char *chat_id, int max_msgs)
{
    cache_ensure(chat_id);
    return extract_tail(s_cache_arr, max_msgs);
}

esp_err_t session_trim(const char *chat_id, int max_msgs)
{
    if (max_msgs <= 0) max_msgs = LANG_SESSION_MAX_MSGS;

    char path[96];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;

    /* Count lines and track byte offsets of last max_msgs lines */
    long *ring = ps_calloc(max_msgs + 1, sizeof(long));
    if (!ring) {
        fclose(f);
        ESP_LOGW(TAG, "session_trim: OOM for offset ring");
        return ESP_ERR_NO_MEM;
    }

    int  ridx     = 0;
    int  count    = 0;
    long pos      = 0;
    bool at_start = true;

    char scratch[512];
    size_t n;
    while ((n = fread(scratch, 1, sizeof(scratch), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (at_start) {
                ring[ridx] = pos;
                ridx = (ridx + 1) % (max_msgs + 1);
                at_start = false;
            }
            if (scratch[i] == '\n') {
                count++;
                at_start = true;
            }
            pos++;
        }
    }
    long file_size = pos;
    fclose(f);

    if (count <= max_msgs) {
        free(ring);
        return ESP_OK;
    }

    long keep_from = ring[(ridx + 1) % (max_msgs + 1)];
    long keep_size = file_size - keep_from;
    free(ring);

    if (keep_size <= 0) return ESP_OK;

    char *buf = ps_malloc((size_t)keep_size + 1);
    if (!buf) {
        ESP_LOGW(TAG, "session_trim: OOM (need %ld bytes)", keep_size);
        return ESP_ERR_NO_MEM;
    }

    f = fopen(path, "r");
    if (!f) { free(buf); return ESP_FAIL; }
    fseek(f, keep_from, SEEK_SET);
    size_t rd = fread(buf, 1, (size_t)keep_size, f);
    fclose(f);

    remove(path);
    f = fopen(path, "w");
    if (!f) { free(buf); return ESP_FAIL; }
    fwrite(buf, 1, rd, f);
    fclose(f);
    free(buf);

    ESP_LOGI(TAG, "Session %s trimmed: %d -> %d lines (%ld bytes kept)",
             chat_id, count, max_msgs, (long)rd);

    /* Also trim the hot cache to stay in sync */
    if (s_cache_arr && strcmp(s_cache_chat_id, chat_id) == 0) {
        int cached = cJSON_GetArraySize(s_cache_arr);
        while (cached > max_msgs) {
            cJSON_DeleteItemFromArray(s_cache_arr, 0);
            cached--;
        }
    }

    return ESP_OK;
}

esp_err_t session_summarize_before_trim(const char *chat_id, int max_msgs)
{
    if (max_msgs <= 0) max_msgs = LANG_SESSION_MAX_MSGS;

    cache_ensure(chat_id);
    int total = cJSON_GetArraySize(s_cache_arr);
    int to_drop = total - max_msgs;

    /* Nothing to drop, or only a few messages — not worth summarizing */
    if (to_drop <= 2) return session_trim(chat_id, max_msgs);

    /* Check local LLM availability — skip summarization if down */
    if (!llm_local_health_check()) {
        ESP_LOGW(TAG, "Local LLM unavailable — skipping session summary for %s", chat_id);
        return session_trim(chat_id, max_msgs);
    }

    /* Build conversation excerpt from messages about to be dropped.
     * Cap at ~2KB to keep the summarization prompt small. */
    char *excerpt = ps_calloc(1, 2048);
    if (!excerpt) return session_trim(chat_id, max_msgs);

    int off = 0;
    for (int i = 0; i < to_drop && off < 1900; i++) {
        const cJSON *msg = cJSON_GetArrayItem(s_cache_arr, i);
        const cJSON *role    = cJSON_GetObjectItemCaseSensitive(msg, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!role || !content) continue;
        off += snprintf(excerpt + off, 2048 - off, "%s: %.300s\n",
                        role->valuestring, content->valuestring);
    }

    if (off == 0) {
        free(excerpt);
        return session_trim(chat_id, max_msgs);
    }

    /* Build summarization prompt */
    cJSON *sum_msgs = cJSON_CreateArray();
    cJSON *sum_msg  = cJSON_CreateObject();
    cJSON_AddStringToObject(sum_msg, "role", "user");

    char *prompt = ps_calloc(1, 2560);
    if (!prompt) {
        free(excerpt);
        cJSON_Delete(sum_msgs);
        return session_trim(chat_id, max_msgs);
    }
    snprintf(prompt, 2560,
        "Summarise these conversation messages in 2-3 concise sentences. "
        "Focus on key facts, decisions, and preferences expressed. "
        "Do not include greetings or filler.\n\n%s", excerpt);
    cJSON_AddStringToObject(sum_msg, "content", prompt);
    cJSON_AddItemToArray(sum_msgs, sum_msg);

    /* Call local LLM */
    llm_set_request_override("ollama", llm_get_local_text_model());
    llm_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = llm_chat_tools(
        "You extract key information from conversations into brief summaries.",
        sum_msgs, NULL, false, &resp);

    if (err == ESP_OK && resp.text && resp.text[0]) {
        /* Prepend chat_id context and save to daily notes */
        char *note = ps_calloc(1, 1024);
        if (note) {
            snprintf(note, 1024, "[Session %s summary] %s", chat_id, resp.text);
            memory_append_today(note);
            ESP_LOGI(TAG, "Session %s: saved summary (%d dropped msgs)", chat_id, to_drop);
            free(note);
        }
    } else {
        ESP_LOGW(TAG, "Session summary LLM call failed for %s: %s",
                 chat_id, esp_err_to_name(err));
    }

    llm_response_free(&resp);
    llm_clear_request_override();
    cJSON_Delete(sum_msgs);
    free(prompt);
    free(excerpt);

    return session_trim(chat_id, max_msgs);
}

esp_err_t session_clear(const char *chat_id)
{
    /* Invalidate cache if it matches */
    if (s_cache_arr && strcmp(s_cache_chat_id, chat_id) == 0) {
        cache_invalidate();
    }

    char path[96];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void session_cache_invalidate(const char *chat_id)
{
    if (!chat_id || !chat_id[0]) {
        cache_invalidate();
        return;
    }
    if (s_cache_arr && strcmp(s_cache_chat_id, chat_id) == 0) {
        cache_invalidate();
        ESP_LOGI(TAG, "Session cache invalidated for %s", chat_id);
    }
}

void session_list(void)
{
    DIR *dir = opendir(LANG_LFS_SESSION_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open sessions directory: %s", LANG_LFS_SESSION_DIR);
        return;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
