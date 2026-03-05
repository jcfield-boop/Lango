#include "serial_cli.h"
#include "langoustine_config.h"
#include "wifi/wifi_manager.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"
#include "ota/ota_manager.h"
#include "audio/stt_client.h"
#include "audio/tts_client.h"
#include "telegram/telegram_bot.h"
#include "camera/uvc_camera.h"
#include "memory/psram_alloc.h"
#include "gateway/ws_server.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("LLM API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = telegram_set_token(tg_token_args.token->sval[0]);
    if (err == ESP_OK) {
        printf("Telegram bot token saved. Polling will use new token immediately.\n");
    } else {
        printf("Failed to save Telegram token: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

/* --- stt_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} stt_key_args;

static int cmd_stt_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&stt_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, stt_key_args.end, argv[0]);
        return 1;
    }
    stt_set_api_key(stt_key_args.key->sval[0]);
    printf("STT API key saved.\n");
    return 0;
}

/* --- tts_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} tts_key_args;

static int cmd_tts_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tts_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tts_key_args.end, argv[0]);
        return 1;
    }
    tts_set_api_key(tts_key_args.key->sval[0]);
    printf("TTS API key saved.\n");
    return 0;
}

/* --- tts_voice command --- */
static struct {
    struct arg_str *voice;
    struct arg_end *end;
} tts_voice_args;

static int cmd_tts_voice(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tts_voice_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tts_voice_args.end, argv[0]);
        return 1;
    }
    tts_set_voice(tts_voice_args.voice->sval[0]);
    printf("TTS voice set to: %s\n", tts_voice_args.voice->sval[0]);
    return 0;
}

/* --- tts_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} tts_model_args;

static int cmd_tts_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tts_model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tts_model_args.end, argv[0]);
        return 1;
    }
    tts_set_model(tts_model_args.model->sval[0]);
    printf("TTS model set to: %s\n", tts_model_args.model->sval[0]);
    return 0;
}

/* --- say command (TTS + I2S playback) --- */
static int cmd_say(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: say <text to speak>\n");
        return 1;
    }
    /* Join all args into one string */
    char text[512] = {0};
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(text) - 1; i++) {
        if (i > 1 && pos < sizeof(text) - 2) text[pos++] = ' ';
        size_t len = strlen(argv[i]);
        if (pos + len >= sizeof(text)) len = sizeof(text) - pos - 1;
        memcpy(text + pos, argv[i], len);
        pos += len;
    }
    char id_out[9] = {0};
    printf("Generating TTS for: \"%s\"\n", text);
    esp_err_t ret = tts_generate(text, id_out);
    if (ret == ESP_OK) {
        printf("Played TTS (id=%s)\n", id_out);
    } else {
        printf("TTS failed: %s\n", esp_err_to_name(ret));
    }
    return (ret == ESP_OK) ? 0 : 1;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = ps_malloc(4096);
    if (!buf) { printf("Out of memory.\n"); return 1; }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- skill_list command --- */
static int cmd_skill_list(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *buf = ps_malloc(4096);
    if (!buf) { printf("Out of memory.\n"); return 1; }
    size_t n = skill_loader_build_summary(buf, 4096);
    if (n == 0) {
        printf("No skills found under %s.\n", LANG_LFS_SKILLS_DIR);
    } else {
        printf("=== Skills ===\n%s", buf);
    }
    free(buf);
    return 0;
}

/* --- skill_show command --- */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_show_args;

static bool has_md_suffix(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3) && strcmp(name + len - 3, ".md") == 0;
}

static bool build_skill_path(const char *name, char *out, size_t out_size)
{
    if (!name || !name[0]) return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;

    if (has_md_suffix(name)) {
        snprintf(out, out_size, "%s/%s", LANG_LFS_SKILLS_DIR, name);
    } else {
        snprintf(out, out_size, "%s/%s.md", LANG_LFS_SKILLS_DIR, name);
    }
    return true;
}

static int cmd_skill_show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_show_args.end, argv[0]);
        return 1;
    }

    char path[128];
    if (!build_skill_path(skill_show_args.name->sval[0], path, sizeof(path))) {
        printf("Invalid skill name.\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Skill not found: %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    fclose(f);
    printf("\n============\n");
    return 0;
}

/* --- skill_search command --- */
static struct {
    struct arg_str *keyword;
    struct arg_end *end;
} skill_search_args;

static bool contains_nocase(const char *text, const char *keyword)
{
    if (!text || !keyword || !keyword[0]) return false;
    size_t key_len = strlen(keyword);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < key_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) {
            i++;
        }
        if (i == key_len) return true;
    }
    return false;
}

static int cmd_skill_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_search_args.end, argv[0]);
        return 1;
    }

    const char *keyword = skill_search_args.keyword->sval[0];
    DIR *dir = opendir(LANG_LFS_SKILLS_DIR);
    if (!dir) {
        printf("Cannot open %s.\n", LANG_LFS_SKILLS_DIR);
        return 1;
    }

    int matches = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);
        if (name_len < 4 || strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[280];
        snprintf(full_path, sizeof(full_path), "%s/%s", LANG_LFS_SKILLS_DIR, name);

        bool file_matched = contains_nocase(name, keyword);
        int  matched_line = 0;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int  line_no = 0;
        while (!file_matched && fgets(line, sizeof(line), f)) {
            line_no++;
            if (contains_nocase(line, keyword)) {
                file_matched = true;
                matched_line = line_no;
            }
        }
        fclose(f);

        if (file_matched) {
            matches++;
            if (matched_line > 0) {
                printf("- %s (matched at line %d)\n", full_path, matched_line);
            } else {
                printf("- %s (matched in filename)\n", full_path);
            }
        }
    }
    closedir(dir);

    if (matches == 0) {
        printf("No skills matched keyword: %s\n", keyword);
    } else {
        printf("Total matches: %d\n", matches);
    }
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source  = "not set";
    const char *display = "(empty)";

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source  = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    if (strcmp(source, "not set") == 0 && build_val && build_val[0]) {
        source  = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-16s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-16s: %s  [%s]\n", label, display, source);
    }
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Langoustine Configuration ===\n");
    print_config("WiFi SSID",    LANG_NVS_WIFI,   LANG_NVS_KEY_SSID,     LANG_SECRET_WIFI_SSID,    false);
    print_config("WiFi Pass",    LANG_NVS_WIFI,   LANG_NVS_KEY_PASS,     LANG_SECRET_WIFI_PASS,    true);
    print_config("LLM API Key",  LANG_NVS_LLM,    LANG_NVS_KEY_API_KEY,  LANG_SECRET_API_KEY,      true);
    print_config("LLM Model",    LANG_NVS_LLM,    LANG_NVS_KEY_MODEL,    LANG_SECRET_MODEL,        false);
    print_config("LLM Provider", LANG_NVS_LLM,    LANG_NVS_KEY_PROVIDER, LANG_SECRET_MODEL_PROVIDER, false);
    print_config("Telegram Token", LANG_NVS_TG,   LANG_NVS_KEY_TG_TOKEN, LANG_SECRET_TG_TOKEN,     true);
    print_config("STT Key",      LANG_NVS_STT,    LANG_NVS_KEY_API_KEY,  "",                       true);
    print_config("TTS Key",      LANG_NVS_TTS,    LANG_NVS_KEY_API_KEY,  "",                       true);
    print_config("TTS Voice",    LANG_NVS_TTS,    LANG_NVS_KEY_VOICE,    LANG_DEFAULT_TTS_VOICE,   false);
    print_config("Proxy Host",   LANG_NVS_PROXY,  LANG_NVS_KEY_PROXY_HOST, LANG_SECRET_PROXY_HOST, false);
    print_config("Proxy Port",   LANG_NVS_PROXY,  LANG_NVS_KEY_PROXY_PORT, LANG_SECRET_PROXY_PORT, false);
    print_config("Search Key",   LANG_NVS_SEARCH, LANG_NVS_KEY_API_KEY,  LANG_SECRET_SEARCH_KEY,   true);
    printf("=================================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        LANG_NVS_WIFI, LANG_NVS_LLM, LANG_NVS_TG, LANG_NVS_STT, LANG_NVS_TTS,
        LANG_NVS_PROXY, LANG_NVS_SEARCH, NULL
    };
    for (int i = 0; namespaces[i]; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    printf("Checking HEARTBEAT.md...\n");
    if (heartbeat_trigger()) {
        printf("Heartbeat: agent prompted with pending tasks.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
    return 0;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }
    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

/* --- capture command --- */
static int cmd_capture(int argc, char **argv)
{
    if (!uvc_camera_is_connected()) {
        printf("No USB webcam detected.\n");
        return 1;
    }

    uint8_t *buf = ps_malloc(LANG_CAMERA_BUF_SIZE);
    if (!buf) {
        printf("Out of PSRAM.\n");
        return 1;
    }

    printf("Capturing...\n");
    size_t jpeg_len = 0;
    esp_err_t err = uvc_camera_capture(buf, LANG_CAMERA_BUF_SIZE,
                                       &jpeg_len, LANG_CAMERA_CAPTURE_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("Capture failed: %s\n", esp_err_to_name(err));
        free(buf);
        return 1;
    }

    /* Save to LittleFS */
    struct stat st;
    if (stat(LANG_CAMERA_CAPTURE_DIR, &st) != 0) {
        mkdir(LANG_CAMERA_CAPTURE_DIR, 0755);
    }
    FILE *f = fopen(LANG_CAMERA_CAPTURE_PATH, "wb");
    if (f) {
        fwrite(buf, 1, jpeg_len, f);
        fclose(f);
        printf("Captured %u bytes → %s\n", (unsigned)jpeg_len, LANG_CAMERA_CAPTURE_PATH);
    } else {
        printf("Captured %u bytes but failed to save.\n", (unsigned)jpeg_len);
    }

    free(buf);
    return 0;
}

/* --- tool_exec command --- */
static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }
    const char *tool_name  = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = ps_malloc(4096);
    if (!output) { printf("Out of memory.\n"); return 1; }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- ota command --- */
static struct {
    struct arg_str *url;
    struct arg_end *end;
} ota_args;

static int cmd_ota(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ota_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ota_args.end, argv[0]);
        return 1;
    }
    const char *url = ota_args.url->sval[0];
    printf("Starting OTA from: %s\n", url);
    printf("Progress will appear in the live log. Device will reboot on success.\n");
    esp_err_t err = ota_update_from_url(url);
    if (err != ESP_OK) {
        printf("OTA failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;
}

/* --- set_http_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} http_token_args;

static int cmd_set_http_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&http_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, http_token_args.end, argv[0]);
        return 1;
    }
    ws_server_set_auth_token(http_token_args.token->sval[0]);
    printf("HTTP auth token set.\n");
    return 0;
}

/* --- set_cors_origin command --- */
static struct {
    struct arg_str *origin;
    struct arg_end *end;
} cors_origin_args;

static int cmd_set_cors_origin(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&cors_origin_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, cors_origin_args.end, argv[0]);
        return 1;
    }
    ws_server_set_cors_origin(cors_origin_args.origin->sval[0]);
    printf("CORS origin set to: %s\n", cors_origin_args.origin->sval[0]);
    return 0;
}

/* --- log_level command --- */
static struct {
    struct arg_str *tag;
    struct arg_int *level;
    struct arg_end *end;
} log_level_args;

static int cmd_log_level(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&log_level_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, log_level_args.end, argv[0]);
        return 1;
    }
    const char *tag = log_level_args.tag->sval[0];
    int level = log_level_args.level->ival[0];
    if (level < 0 || level > 5) {
        printf("Level must be 0-5: 0=none 1=error 2=warn 3=info 4=debug 5=verbose\n");
        return 1;
    }
    esp_log_level_set(tag, (esp_log_level_t)level);
    printf("Log level for '%s' set to %d\n", tag, level);
    return 0;
}

/* --- tg_allow / tg_disallow / tg_allowlist commands --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} tg_allow_args;

static int cmd_tg_allow(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_allow_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_allow_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = telegram_add_allowed_id(tg_allow_args.chat_id->sval[0]);
    if (err == ESP_OK) printf("Added to Telegram allowlist.\n");
    else printf("Error: %s\n", esp_err_to_name(err));
    return 0;
}

static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} tg_disallow_args;

static int cmd_tg_disallow(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_disallow_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_disallow_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = telegram_remove_allowed_id(tg_disallow_args.chat_id->sval[0]);
    if (err == ESP_OK) printf("Removed from Telegram allowlist.\n");
    else printf("Not found: %s\n", esp_err_to_name(err));
    return 0;
}

static int cmd_tg_allowlist(int argc, char **argv)
{
    (void)argc; (void)argv;
    telegram_list_allowed_ids();
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lango> ";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 16384;  /* TTS/STT CLI cmds need TLS stack */

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    esp_console_register_help_command();

    /* set_wifi */
    wifi_set_args.ssid     = arg_str1(NULL, NULL, "<ssid>",     "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end      = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "set_wifi",
        .help    = "Set WiFi SSID and password",
        .func    = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status", .help = "Show WiFi status", .func = &cmd_wifi_status
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan", .help = "Scan nearby WiFi APs", .func = &cmd_wifi_scan
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key", .help = "Set LLM API key",
        .func = &cmd_set_api_key, .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end   = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model", .help = "Set LLM model",
        .func = &cmd_set_model, .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "anthropic|openai|openrouter");
    provider_args.end      = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider", .help = "Set LLM provider",
        .func = &cmd_set_model_provider, .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end   = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command  = "set_tg_token",
        .help     = "Set Telegram bot token",
        .func     = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* stt_key */
    stt_key_args.key = arg_str1(NULL, NULL, "<key>", "STT (Whisper) API key");
    stt_key_args.end = arg_end(1);
    esp_console_cmd_t stt_key_cmd = {
        .command  = "stt_key",
        .help     = "Set STT API key (Groq Whisper-compatible)",
        .func     = &cmd_stt_key,
        .argtable = &stt_key_args,
    };
    esp_console_cmd_register(&stt_key_cmd);

    /* tts_key */
    tts_key_args.key = arg_str1(NULL, NULL, "<key>", "TTS API key");
    tts_key_args.end = arg_end(1);
    esp_console_cmd_t tts_key_cmd = {
        .command  = "tts_key",
        .help     = "Set TTS API key (Groq PlayAI-compatible)",
        .func     = &cmd_tts_key,
        .argtable = &tts_key_args,
    };
    esp_console_cmd_register(&tts_key_cmd);

    /* tts_voice */
    tts_voice_args.voice = arg_str1(NULL, NULL, "<voice>", "TTS voice name");
    tts_voice_args.end   = arg_end(1);
    esp_console_cmd_t tts_voice_cmd = {
        .command  = "tts_voice",
        .help     = "Set TTS voice (default: " LANG_DEFAULT_TTS_VOICE ")",
        .func     = &cmd_tts_voice,
        .argtable = &tts_voice_args,
    };
    esp_console_cmd_register(&tts_voice_cmd);

    /* skill_list */
    esp_console_cmd_t skill_list_cmd = {
        .command = "skill_list",
        .help    = "List installed skills",
        .func    = &cmd_skill_list,
    };
    esp_console_cmd_register(&skill_list_cmd);

    /* skill_show */
    skill_show_args.name = arg_str1(NULL, NULL, "<name>", "Skill name (e.g. weather)");
    skill_show_args.end  = arg_end(1);
    esp_console_cmd_t skill_show_cmd = {
        .command  = "skill_show",
        .help     = "Print full content of a skill file",
        .func     = &cmd_skill_show,
        .argtable = &skill_show_args,
    };
    esp_console_cmd_register(&skill_show_cmd);

    /* skill_search */
    skill_search_args.keyword = arg_str1(NULL, NULL, "<keyword>", "Keyword to search");
    skill_search_args.end     = arg_end(1);
    esp_console_cmd_t skill_search_cmd = {
        .command  = "skill_search",
        .help     = "Search skill files by keyword",
        .func     = &cmd_skill_search,
        .argtable = &skill_search_args,
    };
    esp_console_cmd_register(&skill_search_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read", .help = "Read MEMORY.md", .func = &cmd_memory_read
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end     = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command  = "memory_write", .help = "Write to MEMORY.md",
        .func     = &cmd_memory_write, .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list", .help = "List all sessions", .func = &cmd_session_list
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end     = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command  = "session_clear", .help = "Clear a session",
        .func     = &cmd_session_clear, .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info", .help = "Show heap/PSRAM memory", .func = &cmd_heap_info
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command  = "set_search_key", .help = "Set Brave Search API key",
        .func     = &cmd_set_search_key, .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.end  = arg_end(2);
    esp_console_cmd_t proxy_cmd = {
        .command  = "set_proxy", .help = "Set HTTP proxy",
        .func     = &cmd_set_proxy, .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy", .help = "Remove proxy configuration", .func = &cmd_clear_proxy
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show", .help = "Show current configuration", .func = &cmd_config_show
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset", .help = "Clear all NVS overrides", .func = &cmd_config_reset
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger", .help = "Manually trigger heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start", .help = "Start cron scheduler", .func = &cmd_cron_start
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help    = "Execute a registered tool: tool_exec <name> '{...json...}'",
        .func    = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* ota */
    ota_args.url = arg_str1(NULL, NULL, "<url>", "HTTPS URL to firmware .bin");
    ota_args.end = arg_end(1);
    esp_console_cmd_t ota_cmd = {
        .command  = "ota",
        .help     = "Update firmware over WiFi: ota https://host/path/fw.bin",
        .func     = &cmd_ota,
        .argtable = &ota_args,
    };
    esp_console_cmd_register(&ota_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart", .help = "Restart the device", .func = &cmd_restart
    };
    esp_console_cmd_register(&restart_cmd);

    /* capture */
    esp_console_cmd_t capture_cmd = {
        .command = "capture",
        .help    = "Capture JPEG from USB webcam → /lfs/captures/latest.jpg",
        .func    = &cmd_capture,
    };
    esp_console_cmd_register(&capture_cmd);

    /* set_http_token */
    http_token_args.token = arg_str1(NULL, NULL, "<token>", "Bearer token for HTTP auth (empty to disable)");
    http_token_args.end   = arg_end(1);
    esp_console_cmd_t http_token_cmd = {
        .command  = "set_http_token",
        .help     = "Set HTTP bearer auth token (empty string disables auth)",
        .func     = &cmd_set_http_token,
        .argtable = &http_token_args,
    };
    esp_console_cmd_register(&http_token_cmd);

    /* set_cors_origin */
    cors_origin_args.origin = arg_str1(NULL, NULL, "<origin>", "CORS origin URL (e.g. http://192.168.1.5 or *)");
    cors_origin_args.end    = arg_end(1);
    esp_console_cmd_t cors_cmd = {
        .command  = "set_cors_origin",
        .help     = "Set CORS Access-Control-Allow-Origin header",
        .func     = &cmd_set_cors_origin,
        .argtable = &cors_origin_args,
    };
    esp_console_cmd_register(&cors_cmd);

    /* tg_allow */
    tg_allow_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Telegram chat ID to allow");
    tg_allow_args.end     = arg_end(1);
    esp_console_cmd_t tg_allow_cmd = {
        .command = "tg_allow", .help = "Add Telegram chat ID to allowlist",
        .func = &cmd_tg_allow, .argtable = &tg_allow_args,
    };
    esp_console_cmd_register(&tg_allow_cmd);

    /* tg_disallow */
    tg_disallow_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Telegram chat ID to remove");
    tg_disallow_args.end     = arg_end(1);
    esp_console_cmd_t tg_disallow_cmd = {
        .command = "tg_disallow", .help = "Remove Telegram chat ID from allowlist",
        .func = &cmd_tg_disallow, .argtable = &tg_disallow_args,
    };
    esp_console_cmd_register(&tg_disallow_cmd);

    /* tg_allowlist */
    esp_console_cmd_t tg_allowlist_cmd = {
        .command = "tg_allowlist", .help = "Show Telegram chat ID allowlist",
        .func = &cmd_tg_allowlist,
    };
    esp_console_cmd_register(&tg_allowlist_cmd);

    /* tts_model */
    tts_model_args.model = arg_str1(NULL, NULL, "<model>", "TTS model ID");
    tts_model_args.end   = arg_end(1);
    esp_console_cmd_t tts_model_cmd = {
        .command  = "tts_model",
        .help     = "Set TTS model (e.g. canopylabs/orpheus-v1-english)",
        .func     = &cmd_tts_model,
        .argtable = &tts_model_args,
    };
    esp_console_cmd_register(&tts_model_cmd);

    /* say */
    esp_console_cmd_t say_cmd = {
        .command = "say",
        .help    = "Speak text via TTS + I2S speaker: say hello world",
        .func    = &cmd_say,
    };
    esp_console_cmd_register(&say_cmd);

    /* log_level */
    log_level_args.tag   = arg_str1(NULL, NULL, "<tag>",   "Log tag (e.g. tts, stt, agent, llm, ws, audio_pipeline, or * for all)");
    log_level_args.level = arg_int1(NULL, NULL, "<level>", "0=none 1=error 2=warn 3=info 4=debug 5=verbose");
    log_level_args.end   = arg_end(2);
    esp_console_cmd_t log_level_cmd = {
        .command  = "log_level",
        .help     = "Set per-tag log level at runtime (no rebuild needed)",
        .func     = &cmd_log_level,
        .argtable = &log_level_args,
    };
    esp_console_cmd_register(&log_level_cmd);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");
    return ESP_OK;
}
