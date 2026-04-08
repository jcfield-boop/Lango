#include "context_builder.h"
#include "langoustine_config.h"
#include "memory/memory_store.h"
#include "memory/psram_alloc.h"
#include "skills/skill_loader.h"
#include "heartbeat/heartbeat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    /* ── Static prefix (cacheable by LLM providers) ───────────────
     * OpenAI: automatic prefix caching for identical first 256+ tokens
     * Ollama: KV cache reuse requires byte-identical prefix
     * Anthropic: cache_control markers on static blocks
     * IMPORTANT: All static content MUST come before any dynamic content.
     * Moving the timestamp here would break prefix caching on every turn. */

    off += snprintf(buf + off, size - off,
        "# Langoustine\n\n"
        "You are Langoustine, a personal AI assistant running on an ESP32-S3.\n"
        "You communicate primarily via WebSocket and support voice interaction.\n\n"
        "## Response constraints\n"
        "- Keep responses concise — optimized for text-to-speech playback.\n"
        "- Bullet points over prose. Be clear and direct.\n"
        "- For briefings: 5-10 bullets max. No padding or unnecessary caveats.\n\n"
        "## Available Tools\n"
        "- web_search: current facts, news, weather\n"
        "- read_file / write_file / edit_file / list_dir: LittleFS file access (/lfs/)\n"
        "- cron_add / cron_list / cron_remove: scheduled tasks\n"
        "- http_request: HTTPS GET/POST to external APIs\n"
        "- send_email: send email via Gmail SMTP\n"
        "- search_files: grep LittleFS files for text across memory, notes, and skills\n"
        "- system_info: device health — heap, PSRAM, LittleFS, uptime, WiFi RSSI\n"
        "- memory_write: ONLY way to persistently save a user preference, fact, or instruction\n"
        "- memory_append_today: append a note to today's daily log\n"
        "- ha_request: query/control Home Assistant\n"
        "- klipper_request: query/control Klipper 3D printer via Moonraker\n"
        "Use tools when needed.\n\n"
        "## Never answer from training data\n"
        "For anything that changes over time — stock prices, sports scores, weather, news,\n"
        "exchange rates — ALWAYS use web_search. Your training data is stale.\n\n"
        "## Scheduling Rules (cron_add)\n"
        "- The current time is in the 'Current Time' section at the end of this prompt.\n"
        "- For relative times: use seconds_from_now (e.g. 300 for 5 minutes).\n"
        "- For absolute times: compute the epoch offset from the current time.\n\n"
        "## File Paths\n"
        "- /lfs/config/USER.md — user profile (name, timezone, preferences)\n"
        "- /lfs/config/SOUL.md — personality (read-only)\n"
        "- /lfs/config/SERVICES.md — third-party service credentials\n"
        "- /lfs/memory/MEMORY.md — long-term memory\n"
        "- /lfs/memory/<YYYY-MM-DD>.md — today's daily notes\n"
        "- /lfs/skills/<name>.md — skill files\n\n"
        "## Credentials\n"
        "- /lfs/config/SERVICES.md contains API keys for external services.\n"
        "- Read it only when a skill requires it. Never expose credential values.\n\n"
        "## Memory Policy — MANDATORY\n"
        "RULE: You have NO persistent memory between sessions except what is stored via tools.\n"
        "When the user says 'remember', 'save', 'note', or states a preference:\n"
        "  1. Call memory_write immediately — before any other tool or response text.\n"
        "  2. Confirm with the ok:true result, not before it.\n"
        "Confidence >= 0.7 required. Once per conversation turn.\n\n"
        "## Skills\n"
        "When a user request matches a skill, MUST call read_file to load the full skill\n"
        "instructions before taking any action. Skills override improvisation.\n"
        "Create new skills with write_file to /lfs/skills/<name>.md.\n");

    off += snprintf(buf + off, size - off,
        "\n## Language\n\n"
        "Always respond in English. Never switch to another language "
        "unless the user explicitly asks for a translation.\n");

    off = append_file(buf, size, off, LANG_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, LANG_USER_FILE, "User Info");

    /* Single shared PSRAM temp buffer for skills, memory, and recent notes.
     * LANG_MEMORY_MAX_BYTES (16KB) is the largest — reuse for all three. */
    char *tmp_buf = ps_malloc(LANG_MEMORY_MAX_BYTES);
    if (tmp_buf) {
        /* Skills summary (uses ~2KB of the 16KB buffer) */
        size_t skills_len = skill_loader_build_summary(tmp_buf, 2048);
        if (skills_len > 0) {
            off += snprintf(buf + off, size - off,
                "\n## Available Skills\n\n"
                "REQUIRED: call read_file before acting on any matching request:\n%s\n",
                tmp_buf);
        }

        /* Long-term memory */
        if (memory_read_long_term(tmp_buf, LANG_MEMORY_MAX_BYTES) == ESP_OK && tmp_buf[0]) {
            off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", tmp_buf);
        }

        /* Recent daily notes */
        if (memory_read_recent(tmp_buf, 2048, 3) == ESP_OK && tmp_buf[0]) {
            off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", tmp_buf);
        }

        free(tmp_buf);
    }

    /* Today's completed heartbeat tasks (prevents duplicate work) */
    {
        char hb_log[512];
        int n = heartbeat_get_today_log(hb_log, sizeof(hb_log));
        if (n > 0) {
            off += snprintf(buf + off, size - off,
                            "\n## Today's Completed Tasks\n\n%s", hb_log);
        }
    }

    /* ── Dynamic timestamp (MUST be last in system prompt) ────────
     * Placed after all static + semi-static content so that LLM
     * provider prefix caching (OpenAI auto-cache, Ollama KV cache,
     * Anthropic cache_control) can reuse the static prefix across
     * turns. Only the tail changes each turn. */
    {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S %Z (%A)", &local);
        off += snprintf(buf + off, size - off,
                        "\n## Current Time\n\n%s\n", timebuf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}
