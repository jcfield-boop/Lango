#include "tool_search_files.h"
#include "langoustine_config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_search";

#define MAX_MATCHES  20
#define MAX_LINE_LEN 120

static bool icontains(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) break;
        }
        if (j == nlen) return true;
    }
    return false;
}

/* Search all .md / .txt files in a single directory for pattern.
 * Returns updated match count. */
static int search_dir(const char *dir_path, const char *prefix_filter,
                      const char *pattern,
                      char *output, size_t output_size, size_t *out_pos,
                      int matches)
{
    /* Skip console and sessions — too large / not useful */
    const char *base = strrchr(dir_path, '/');
    if (base) {
        base++;
        if (strcmp(base, "console") == 0 || strcmp(base, "sessions") == 0) {
            return matches;
        }
    }

    /* Apply prefix_filter: filter to a specific subdirectory path */
    if (prefix_filter && prefix_filter[0]) {
        if (strstr(dir_path, prefix_filter) == NULL) {
            return matches;
        }
    }

    DIR *dir = opendir(dir_path);
    if (!dir) return matches;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && matches < MAX_MATCHES) {
        if (ent->d_name[0] == '.') continue;

        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        /* Recurse into subdirectories */
        if (ent->d_type == DT_DIR) {
            matches = search_dir(full_path, prefix_filter, pattern,
                                 output, output_size, out_pos, matches);
            continue;
        }

        /* Only search text files */
        size_t nlen = strlen(ent->d_name);
        bool is_text = (nlen >= 4 && strcmp(ent->d_name + nlen - 3, ".md") == 0)
                    || (nlen >= 5 && strcmp(ent->d_name + nlen - 4, ".txt") == 0);
        if (!is_text) continue;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int  linenum = 0;
        while (fgets(line, sizeof(line), f) && matches < MAX_MATCHES) {
            linenum++;
            int len = (int)strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
            line[len] = '\0';

            if (!icontains(line, pattern)) continue;

            char trunc[MAX_LINE_LEN + 4];
            if (len > MAX_LINE_LEN) {
                snprintf(trunc, sizeof(trunc), "%.*s...", MAX_LINE_LEN, line);
            } else {
                strncpy(trunc, line, sizeof(trunc) - 1);
                trunc[sizeof(trunc)-1] = '\0';
            }

            int written = snprintf(output + *out_pos, output_size - *out_pos,
                                   "%s:%d: %s\n", full_path, linenum, trunc);
            if (written > 0 && *out_pos + (size_t)written < output_size - 1) {
                *out_pos += written;
                matches++;
            } else {
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return matches;
}

esp_err_t tool_search_files_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *j_pattern = cJSON_GetObjectItem(input, "pattern");
    cJSON *j_prefix  = cJSON_GetObjectItem(input, "prefix");

    if (!j_pattern || !cJSON_IsString(j_pattern) || !j_pattern->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'pattern' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *pattern       = j_pattern->valuestring;
    const char *prefix_filter = (j_prefix && cJSON_IsString(j_prefix)) ? j_prefix->valuestring : NULL;

    cJSON_Delete(input);

    size_t out_pos = 0;
    int    matches = search_dir(LANG_LFS_BASE, prefix_filter, pattern,
                                output, output_size, &out_pos, 0);

    if (matches == 0) {
        snprintf(output, output_size, "No matches found.");
    } else {
        output[out_pos] = '\0';
        if (matches >= MAX_MATCHES) {
            size_t room = output_size - out_pos;
            if (room > 40) {
                snprintf(output + out_pos, room, "(results capped at %d matches)", MAX_MATCHES);
            }
        }
    }

    ESP_LOGI(TAG, "search '%s' prefix='%s' → %d matches", pattern,
             prefix_filter ? prefix_filter : "(none)", matches);
    return ESP_OK;
}
