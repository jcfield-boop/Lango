#include "skills/skill_loader.h"
#include "langoustine_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read /lfs/memory/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing: date/time, weather, news, pending tasks\n" \
    "6. Log to today's daily note at /lfs/memory/<YYYY-MM-DD>.md\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SELF_TEST \
    "# Self-Test\n" \
    "\n" \
    "Run a validation checklist and report pass/fail for each Langoustine capability.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to run a self-test, system check, or validate that Langoustine is working.\n" \
    "\n" \
    "## How to run\n" \
    "Run each check in order. Report PASS or FAIL for each.\n" \
    "\n" \
    "### T1 — Clock\n" \
    "Call get_current_time. PASS if returns a valid date/time.\n" \
    "\n" \
    "### T2 — Memory read\n" \
    "Call read_file on /lfs/memory/MEMORY.md.\n" \
    "PASS if file exists or returns empty (not an error).\n" \
    "\n" \
    "### T3 — Daily note write\n" \
    "Call write_file with append=true on /lfs/memory/<today>.md.\n" \
    "Content: '- [self-test T3] write ok\\n'\n" \
    "PASS if tool returns 'OK: appended'.\n" \
    "\n" \
    "### T4 — System info\n" \
    "Call system_info. PASS if returns heap and PSRAM stats.\n" \
    "\n" \
    "### T5 — Web search\n" \
    "Call web_search with query 'ESP32 microcontroller'.\n" \
    "PASS if result is non-empty and contains relevant content.\n" \
    "\n" \
    "### T6 — File list\n" \
    "Call list_dir with prefix /lfs/skills/.\n" \
    "PASS if at least weather.md, daily-briefing.md, self-test.md are listed.\n" \
    "\n" \
    "## Output format\n" \
    "- T1 Clock: PASS/FAIL — <detail>\n" \
    "- T2 Memory: PASS/FAIL — <detail>\n" \
    "... etc\n" \
    "End with: 'N/6 tests passed.'\n"

#define BUILTIN_EMAIL \
    "# Email\n" \
    "\n" \
    "Send an email via Gmail using the send_email tool.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to send an email, or when a skill needs to deliver a report by email.\n" \
    "\n" \
    "## How to use\n" \
    "1. Identify subject and body from context\n" \
    "2. Call send_email with subject and body.\n" \
    "3. Report success or failure to the user.\n" \
    "\n" \
    "## SERVICES.md format required\n" \
    "## Email\n" \
    "service: Gmail\n" \
    "smtp_host: smtp.gmail.com\n" \
    "smtp_port: 465\n" \
    "username: you@gmail.com\n" \
    "password: xxxx xxxx xxxx xxxx\n" \
    "from_address: Langoustine <you@gmail.com>\n" \
    "to_address: you@gmail.com\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for Langoustine.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the assistant something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a skill file with: # Title, description, ## When to use, ## How to use\n" \
    "3. Save to /lfs/skills/<name>.md using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "email",          BUILTIN_EMAIL          },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
    { "self-test",      BUILTIN_SELF_TEST      },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

static void install_builtin(const builtin_skill_t *skill)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s.md", LANG_SKILLS_PREFIX, skill->filename);

    FILE *test = fopen(path, "r");
    if (test) {
        fclose(test);
        ESP_LOGD(TAG, "Built-in skill already present: %s", path);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write skill: %s", path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    ESP_LOGI(TAG, "Installed built-in skill: %s", path);
}

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    ESP_LOGI(TAG, "Skills system ready (%d built-in)", (int)NUM_BUILTINS);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        if (off == 0 && line[0] == '\n') continue;

        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

static void extract_when_to_use(FILE *f, char *out, size_t out_size)
{
    char line[256];
    bool in_section = false;
    size_t off = 0;

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';

        if (!in_section) {
            if (len >= 2 && line[0] == '#' && line[1] == '#') {
                const char *h = line + 2;
                while (*h == ' ') h++;
                if (strncasecmp(h, "when to use", 11) == 0) in_section = true;
            }
        } else {
            if (len >= 2 && line[0] == '#' && line[1] == '#') break;
            if (len == 0) continue;

            if (off > 0 && off < out_size - 2) out[off++] = ' ';

            size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
            memcpy(out + off, line, copy);
            off += copy;
        }
    }
    out[off] = '\0';
}

#define SKILL_SUMMARY_SOFT_CAP 1800

size_t skill_loader_build_summary(char *buf, size_t size)
{
    /* LittleFS has real directories — opendir on the skills dir works directly */
    DIR *dir = opendir(LANG_LFS_SKILLS_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open skills dir: %s", LANG_LFS_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (name_len < 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[128];
        snprintf(full_path, sizeof(full_path), "%s%s", LANG_SKILLS_PREFIX, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        char desc[128];
        extract_description(f, desc, sizeof(desc));

        char when[192];
        extract_when_to_use(f, when, sizeof(when));
        fclose(f);

        if (when[0]) {
            off += snprintf(buf + off, size - off,
                "- **%s** [trigger: %s] → MUST call: read_file %s\n",
                title, when, full_path);
        } else {
            off += snprintf(buf + off, size - off,
                "- **%s**: %s → MUST call: read_file %s\n",
                title, desc, full_path);
        }

        if (off >= SKILL_SUMMARY_SOFT_CAP) {
            off += snprintf(buf + off, size - off,
                "(additional skills available — use list_dir on %s to see all)\n",
                LANG_SKILLS_PREFIX);
            break;
        }
    }

    closedir(dir);

    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills summary: %d bytes", (int)off);
    return off;
}
