#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Generic key:value parser for markdown sections in /lfs/config/SERVICES.md.
 *
 * Finds a section by its exact header (e.g. "## Home Assistant"), then extracts
 * key:value pairs until the next ## header or EOF.  Each key:value is matched
 * against a caller-provided descriptor array and copied into the caller's buffer.
 *
 * Usage:
 *   char url[128] = {0}, token[256] = {0};
 *   services_kv_t kvs[] = {
 *       { "ha_url",   url,   sizeof(url)   },
 *       { "ha_token", token, sizeof(token) },
 *   };
 *   bool ok = services_parse_section("## Home Assistant", kvs, 2);
 */

typedef struct {
    const char *key;       /* key name to match (e.g. "ha_url") */
    char       *value;     /* destination buffer (caller-owned) */
    size_t      value_size;/* sizeof destination buffer */
} services_kv_t;

/**
 * Parse a named section from SERVICES.md.
 *
 * @param section_header  Exact section line to match, e.g. "## Email"
 * @param kvs             Array of key-value descriptors
 * @param kv_count        Number of entries in kvs
 * @return true if the section was found (regardless of which keys matched)
 */
bool services_parse_section(const char *section_header,
                            services_kv_t *kvs, int kv_count);
