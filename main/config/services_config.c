#include "services_config.h"
#include "langoustine_config.h"
#include "llm/llm_proxy.h"
#include "audio/stt_client.h"
#include "audio/tts_client.h"
#include "tools/tool_web_search.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "services_cfg";

#define SERVICES_PATH  "/lfs/config/SERVICES.md"
#define LINE_MAX       256
#define VALUE_MAX      192

/* ── Webhook secret (stored in RAM, loaded from SERVICES.md) ─── */
static char s_webhook_secret[128] = {0};

const char *services_get_webhook_secret(void)
{
    return s_webhook_secret[0] ? s_webhook_secret : NULL;
}

/* ── Tiny key:value parser for Markdown-ish config ───────────── */

/** Trim leading/trailing whitespace in-place. Returns trimmed start pointer. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

esp_err_t services_config_load(void)
{
    FILE *f = fopen(SERVICES_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "%s not found — skipping", SERVICES_PATH);
        return ESP_OK;  /* not an error — file is optional */
    }

    char line[LINE_MAX];
    char section[64] = {0};   /* current ## heading (lowercase) */
    int  keys_applied = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);

        /* Section header: "## Something" */
        if (p[0] == '#' && p[1] == '#' && p[2] == ' ') {
            char *hdr = trim(p + 3);
            /* Lowercase the section name for easier matching */
            strncpy(section, hdr, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            for (char *c = section; *c; c++) *c = (char)tolower((unsigned char)*c);
            continue;
        }

        /* Skip comment lines and blank lines */
        if (p[0] == '#' || p[0] == '\0') continue;

        /* Key:value — split on first ':' */
        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';

        char *key = trim(p);
        char *val = trim(colon + 1);

        /* Skip empty values */
        if (val[0] == '\0') continue;

        /* ── LLM section ─────────────────────────────────────── */
        if (strstr(section, "llm")) {
            if (strcmp(key, "api_key") == 0) {
                const char *cur = llm_get_api_key();
                if (!cur || cur[0] == '\0') {
                    llm_set_api_key(val);
                    ESP_LOGI(TAG, "LLM api_key loaded from SERVICES.md");
                    keys_applied++;
                }
            } else if (strcmp(key, "provider") == 0) {
                const char *cur = llm_get_provider();
                if (!cur || cur[0] == '\0') {
                    llm_set_provider(val);
                    ESP_LOGI(TAG, "LLM provider set to: %s", val);
                    keys_applied++;
                }
            } else if (strcmp(key, "model") == 0) {
                const char *cur = llm_get_model();
                if (!cur || cur[0] == '\0') {
                    llm_set_model(val);
                    ESP_LOGI(TAG, "LLM model set to: %s", val);
                    keys_applied++;
                }
            }

        /* ── Local Model (Ollama) section ────────────────────── */
        } else if (strstr(section, "local model")) {
            if (strcmp(key, "base_url") == 0) {
                llm_set_local_url(val);
                ESP_LOGI(TAG, "Local model URL: %s", val);
                keys_applied++;
            } else if (strcmp(key, "api_key") == 0) {
                /* Store local API key — used when provider=ollama */
                /* (key is typically "ollama" for Ollama) */
                ESP_LOGI(TAG, "Local model api_key loaded");
                keys_applied++;
            } else if (strcmp(key, "model") == 0) {
                llm_set_local_model(val);
                ESP_LOGI(TAG, "Local model (vision): %s", val);
                keys_applied++;
            } else if (strcmp(key, "local_text_model") == 0) {
                llm_set_local_text_model(val);
                ESP_LOGI(TAG, "Local text model: %s", val);
                keys_applied++;
            } else if (strcmp(key, "voice_provider") == 0) {
                llm_set_voice_provider(val);
                ESP_LOGI(TAG, "Voice provider: %s", val);
                keys_applied++;
            } else if (strcmp(key, "voice_model") == 0) {
                llm_set_voice_model(val);
                ESP_LOGI(TAG, "Voice model: %s", val);
                keys_applied++;
            } else if (strcmp(key, "local_provider") == 0) {
                llm_set_local_provider(val);
                ESP_LOGI(TAG, "Local provider: %s", val);
                keys_applied++;
            } else if (strcmp(key, "system_provider") == 0) {
                llm_set_system_provider(val);
                ESP_LOGI(TAG, "System channel provider: %s", val);
                keys_applied++;
            } else if (strcmp(key, "system_model") == 0) {
                llm_set_system_model(val);
                ESP_LOGI(TAG, "System channel model: %s", val);
                keys_applied++;
            }

        /* ── Speech-to-Text section ──────────────────────────── */
        } else if (strstr(section, "speech-to-text") || strstr(section, "stt")) {
            if (strcmp(key, "stt_key") == 0 || strcmp(key, "api_key") == 0) {
                char masked[32] = {0};
                stt_get_api_key_masked(masked, sizeof(masked));
                if (strcmp(masked, "(not set)") == 0) {
                    stt_set_api_key(val);
                    ESP_LOGI(TAG, "STT api_key loaded from SERVICES.md");
                    keys_applied++;
                }
            }

        /* ── Text-to-Speech section ──────────────────────────── */
        } else if (strstr(section, "text-to-speech") || strstr(section, "tts")) {
            if (strcmp(key, "tts_key") == 0 || strcmp(key, "api_key") == 0) {
                char masked[32] = {0};
                tts_get_api_key_masked(masked, sizeof(masked));
                if (strcmp(masked, "(not set)") == 0) {
                    tts_set_api_key(val);
                    ESP_LOGI(TAG, "TTS api_key loaded from SERVICES.md");
                    keys_applied++;
                }
            } else if (strcmp(key, "tts_voice") == 0 || strcmp(key, "voice") == 0) {
                char voice[64] = {0};
                tts_get_voice(voice, sizeof(voice));
                if (voice[0] == '\0') {
                    tts_set_voice(val);
                    ESP_LOGI(TAG, "TTS voice set to: %s", val);
                    keys_applied++;
                }
            }

        /* ── Web Search section ──────────────────────────────── */
        } else if (strstr(section, "web search") || strstr(section, "search")) {
            if (strcmp(key, "search_key") == 0 || strcmp(key, "api_key") == 0) {
                const char *cur = tool_web_search_get_key();
                if (!cur || cur[0] == '\0') {
                    tool_web_search_set_key(val);
                    ESP_LOGI(TAG, "Web search api_key loaded from SERVICES.md");
                    keys_applied++;
                }
            }

        /* ── Apfel (Apple Foundation Model) ──────────────────── */
        } else if (strstr(section, "apfel") || strstr(section, "apple foundation")) {
            if (strcmp(key, "base_url") == 0) {
                llm_set_apfel_url(val);
                ESP_LOGI(TAG, "Apfel URL: %s", val);
                keys_applied++;
            } else if (strcmp(key, "model") == 0) {
                llm_set_apfel_model(val);
                ESP_LOGI(TAG, "Apfel model: %s", val);
                keys_applied++;
            }

        /* ── Local Audio (mlx-audio / Piper / whisper.cpp) ────── */
        } else if (strstr(section, "local audio")) {
            if (strcmp(key, "base_url") == 0) {
                tts_set_local_url(val);
                stt_set_local_url(val);
                ESP_LOGI(TAG, "Local audio URL: %s (TTS+STT)", val);
                keys_applied++;
            } else if (strcmp(key, "local_model") == 0) {
                tts_set_local_model(val);
                ESP_LOGI(TAG, "Local TTS model: %s", val);
                keys_applied++;
            } else if (strcmp(key, "local_voice") == 0) {
                tts_set_local_voice(val);
                ESP_LOGI(TAG, "Local TTS voice: %s", val);
                keys_applied++;
            }

        /* ── Webhooks ─────────────────────────────────────────── */
        } else if (strstr(section, "webhook")) {
            if (strcmp(key, "webhook_secret") == 0 || strcmp(key, "secret") == 0) {
                strncpy(s_webhook_secret, val, sizeof(s_webhook_secret) - 1);
                ESP_LOGI(TAG, "Webhook secret loaded (%d chars)", (int)strlen(val));
                keys_applied++;
            }
        }

        /* Email, Home Assistant, Klipper sections are already handled by
         * their respective tools reading SERVICES.md directly via file_read.
         * No need to parse them here. */
    }

    fclose(f);
    ESP_LOGI(TAG, "SERVICES.md parsed: %d keys applied (NVS values take priority)", keys_applied);
    return ESP_OK;
}

/* ── Force-reload: unconditionally apply all values from SERVICES.md ── */
/* Called after the user saves SERVICES.md via the web UI.               */

esp_err_t services_config_reload(void)
{
    FILE *f = fopen(SERVICES_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "reload: %s not found", SERVICES_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    char line[LINE_MAX];
    char section[64] = {0};
    int  keys_applied = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);

        if (p[0] == '#' && p[1] == '#' && p[2] == ' ') {
            char *hdr = trim(p + 3);
            strncpy(section, hdr, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            for (char *c = section; *c; c++) *c = (char)tolower((unsigned char)*c);
            continue;
        }
        if (p[0] == '#' || p[0] == '\0') continue;

        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = trim(p);
        char *val = trim(colon + 1);
        if (val[0] == '\0') continue;  /* blank value → skip */

        /* ── LLM ─────────────────────────────────────────────── */
        if (strstr(section, "llm")) {
            if (strcmp(key, "api_key") == 0)  { llm_set_api_key(val);  keys_applied++; }
            else if (strcmp(key, "provider") == 0) { llm_set_provider(val); keys_applied++; }
            else if (strcmp(key, "model") == 0)    { llm_set_model(val);    keys_applied++; }

        /* ── Local Model (Ollama) ────────────────────────────── */
        } else if (strstr(section, "local model")) {
            if (strcmp(key, "base_url") == 0)             { llm_set_local_url(val);        keys_applied++; }
            else if (strcmp(key, "model") == 0)            { llm_set_local_model(val);      keys_applied++; }
            else if (strcmp(key, "local_text_model") == 0) { llm_set_local_text_model(val); keys_applied++; }
            else if (strcmp(key, "voice_provider") == 0)   { llm_set_voice_provider(val);   keys_applied++; }
            else if (strcmp(key, "voice_model") == 0)      { llm_set_voice_model(val);      keys_applied++; }
            else if (strcmp(key, "local_provider") == 0)   { llm_set_local_provider(val);   keys_applied++; }
            else if (strcmp(key, "system_provider") == 0)  { llm_set_system_provider(val);  keys_applied++; }
            else if (strcmp(key, "system_model") == 0)     { llm_set_system_model(val);     keys_applied++; }

        /* ── STT ─────────────────────────────────────────────── */
        } else if (strstr(section, "speech-to-text") || strstr(section, "stt")) {
            if (strcmp(key, "stt_key") == 0 || strcmp(key, "api_key") == 0) {
                stt_set_api_key(val); keys_applied++;
            }

        /* ── TTS ─────────────────────────────────────────────── */
        } else if (strstr(section, "text-to-speech") || strstr(section, "tts")) {
            if (strcmp(key, "tts_key") == 0 || strcmp(key, "api_key") == 0) {
                tts_set_api_key(val); keys_applied++;
            } else if (strcmp(key, "tts_voice") == 0 || strcmp(key, "voice") == 0) {
                tts_set_voice(val); keys_applied++;
            }

        /* ── Web Search ──────────────────────────────────────── */
        } else if (strstr(section, "web search") || strstr(section, "search")) {
            if (strcmp(key, "search_key") == 0 || strcmp(key, "api_key") == 0) {
                tool_web_search_set_key(val); keys_applied++;
            }

        /* ── Apfel (Apple Foundation Model) ──────────────────── */
        } else if (strstr(section, "apfel") || strstr(section, "apple foundation")) {
            if (strcmp(key, "base_url") == 0)        { llm_set_apfel_url(val);   keys_applied++; }
            else if (strcmp(key, "model") == 0)       { llm_set_apfel_model(val); keys_applied++; }

        /* ── Local Audio ─────────────────────────────────────── */
        } else if (strstr(section, "local audio")) {
            if (strcmp(key, "base_url") == 0) {
                tts_set_local_url(val); stt_set_local_url(val); keys_applied++;
            } else if (strcmp(key, "local_model") == 0) {
                tts_set_local_model(val); keys_applied++;
            } else if (strcmp(key, "local_voice") == 0) {
                tts_set_local_voice(val); keys_applied++;
            }

        /* ── Webhooks ─────────────────────────────────────────── */
        } else if (strstr(section, "webhook")) {
            if (strcmp(key, "webhook_secret") == 0 || strcmp(key, "secret") == 0) {
                strncpy(s_webhook_secret, val, sizeof(s_webhook_secret) - 1);
                keys_applied++;
            }
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "SERVICES.md reloaded: %d keys applied (forced, overrides NVS)", keys_applied);
    return ESP_OK;
}
