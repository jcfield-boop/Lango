#include "services_parser.h"

#include <stdio.h>
#include <string.h>

#define SERVICES_PATH "/lfs/config/SERVICES.md"

bool services_parse_section(const char *section_header,
                            services_kv_t *kvs, int kv_count)
{
    FILE *f = fopen(SERVICES_PATH, "r");
    if (!f) return false;

    char line[320];
    bool in_section = false;
    bool found      = false;

    while (fgets(line, sizeof(line), f)) {
        /* Trim trailing whitespace */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' '))
            line[--len] = '\0';

        if (strcmp(line, section_header) == 0) {
            in_section = true;
            found      = true;
            continue;
        }
        if (in_section && len > 1 && line[0] == '#') break;
        if (!in_section || len == 0 || line[0] == '#') continue;

        /* Split on first ':' */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *key = line;
        const char *val = colon + 1;
        while (*val == ' ') val++;

        for (int i = 0; i < kv_count; i++) {
            if (strcmp(key, kvs[i].key) == 0 && kvs[i].value && kvs[i].value_size > 0) {
                strncpy(kvs[i].value, val, kvs[i].value_size - 1);
                kvs[i].value[kvs[i].value_size - 1] = '\0';
                break;
            }
        }
    }

    fclose(f);
    return found;
}
