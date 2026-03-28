#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include <time.h>
#include <sys/time.h>

#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "memory/psram_alloc.h"
#include "cJSON.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"
#include "rules/rule_engine.h"
#include "telegram/telegram_bot.h"
#include "audio/audio_pipeline.h"
#include "audio/stt_client.h"
#include "audio/tts_client.h"
#include "audio/i2s_audio.h"
#include "audio/microphone.h"
#include "audio/wake_word.h"
#include "audio/uac_microphone.h"
#include "camera/uvc_camera.h"
#include "led/led_indicator.h"
#include "display/oled_display.h"
#include "config/services_config.h"
#include "diag/log_buffer.h"
#include "driver/i2c_master.h"
#include "wifi/wifi_onboard.h"
#include "mdns.h"
#include "esp_ota_ops.h"

static const char *TAG = "langoustine";

/* Set system clock to compile time if SNTP fails. */
static void set_time_from_build(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    struct tm t = {0};
    strptime(desc->date, "%b %d %Y", &t);
    strptime(desc->time, "%H:%M:%S", &t);
    t.tm_isdst = -1;
    time_t build_time = mktime(&t);
    if (build_time > 0) {
        struct timeval tv = { .tv_sec = build_time };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "Using build timestamp as clock: %s %s", desc->date, desc->time);
    }
}

/* Write a crash entry to /lfs/memory/crashlog.md if the last reset was abnormal.
 * Called after LittleFS is mounted and bootstrap_dirs() has run.
 * Uses the build timestamp as a minimum clock since NTP is not yet synced. */
#define CRASHLOG_PATH     LANG_LFS_MEMORY_DIR "/crashlog.md"
#define CRASHLOG_MAX_BYTES 4096

static void log_crash_if_needed(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char *reason_str;
    switch (reason) {
        case ESP_RST_PANIC:    reason_str = "panic";     break;
        case ESP_RST_INT_WDT:  reason_str = "int_wdt";   break;
        case ESP_RST_TASK_WDT: reason_str = "task_wdt";  break;
        case ESP_RST_WDT:      reason_str = "wdt";        break;
        case ESP_RST_BROWNOUT: reason_str = "brownout";   break;
        default:               return;  /* Normal reset — nothing to log */
    }

    /* Set clock to build time (minimum timestamp; NTP will correct it later) */
    set_time_from_build();
    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M", &tm_info);

    const esp_app_desc_t *desc = esp_app_get_description();

    /* Trim file if it's grown too large */
    struct stat st;
    if (stat(CRASHLOG_PATH, &st) == 0 && (size_t)st.st_size > CRASHLOG_MAX_BYTES) {
        FILE *tf = fopen(CRASHLOG_PATH, "w");
        if (tf) {
            fprintf(tf, "# Crash Log (trimmed at %s)\n\n", ts);
            fprintf(tf, "| Timestamp (build-approx) | Reason | Firmware |\n");
            fprintf(tf, "|---|---|---|\n");
            fclose(tf);
        }
    }

    /* Create file with header if it doesn't exist */
    if (stat(CRASHLOG_PATH, &st) != 0) {
        FILE *hf = fopen(CRASHLOG_PATH, "w");
        if (hf) {
            fprintf(hf, "# Crash Log\n\n");
            fprintf(hf, "| Timestamp (build-approx) | Reason | Firmware |\n");
            fprintf(hf, "|---|---|---|\n");
            fclose(hf);
        }
    }

    FILE *f = fopen(CRASHLOG_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "crashlog: open failed");
        return;
    }
    fprintf(f, "| %s | **%s** | v%s (%s %s) |\n",
            ts, reason_str, desc->version, desc->date, desc->time);
    fclose(f);

    ESP_LOGW(TAG, "CRASH LOGGED: reason=%s firmware=v%s built=%s %s",
             reason_str, desc->version, desc->date, desc->time);
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LANG_LFS_BASE,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "LittleFS: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

/* Ensure required directories exist (LittleFS supports real directories) */
static void bootstrap_dirs(void)
{
    const char *dirs[] = {
        LANG_LFS_CONFIG_DIR,
        LANG_LFS_MEMORY_DIR,
        LANG_LFS_SESSION_DIR,
        LANG_LFS_SKILLS_DIR,
        LANG_LFS_TTS_CACHE_DIR,
        "/lfs/console",
        LANG_CAMERA_CAPTURE_DIR,
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) {
            mkdir(dirs[i], 0755);
            ESP_LOGI(TAG, "Created dir: %s", dirs[i]);
        }
    }
}

/* Write default content only if the file does not already exist. */
static void write_if_missing(const char *path, const char *content)
{
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return;
    }
    f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create default: %s", path);
        return;
    }
    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Created default: %s", path);
}

static void bootstrap_defaults(void)
{
    write_if_missing(LANG_SOUL_FILE,
        "I am Langoustine, a personal AI assistant running on an ESP32-S3.\n"
        "\n"
        "Personality:\n"
        "- Helpful and friendly\n"
        "- Concise and to the point\n"
        "- Voice-first: optimized for spoken conversation\n"
        "\n"
        "Values:\n"
        "- Accuracy over speed\n"
        "- User privacy and safety\n"
        "- Transparency in actions\n");

    write_if_missing(LANG_USER_FILE,
        "# User Profile\n"
        "\n"
        "- Name: (not set)\n"
        "- Language: English\n"
        "- Timezone: (not set)\n");

    write_if_missing(LANG_HEARTBEAT_FILE,
        "# Heartbeat Tasks\n"
        "\n"
        "- [ ] Get the current time and write a one-line system status entry to today's daily note.\n");
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        lang_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, LANG_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, LANG_CHAN_TELEGRAM) == 0) {
            esp_err_t tg_err = telegram_send_message(msg.chat_id, msg.content);
            if (tg_err != ESP_OK) {
                ESP_LOGW(TAG, "TG send failed for %s", msg.chat_id);
            }
        } else if (strcmp(msg.channel, LANG_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
            char mon[200];
            snprintf(mon, sizeof(mon), "[%.20s] %.160s", msg.chat_id, msg.content);
            for (char *p = mon; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            ws_server_broadcast_monitor("sys", mon);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Route ALL cJSON allocations to PSRAM.
     * cJSON creates many small nodes (title, content strings, array entries)
     * which each fall below the 4KB SPIRAM_MALLOC_ALWAYSINTERNAL threshold and
     * land in SRAM by default. Over a single large LLM turn (28K tokens = ~200+
     * cJSON nodes for message history + tool results), SRAM drops 20-30KB.
     * Redirecting to ps_malloc keeps SRAM stable regardless of context size. */
    static const cJSON_Hooks cjson_psram = { .malloc_fn = ps_malloc, .free_fn = free };
    cJSON_InitHooks((cJSON_Hooks *)&cjson_psram);

    /* Disable the hardware brownout detector.
     * CONFIG_ESP_BROWNOUT_DET=n does NOT clear RTC_CNTL_BROWN_OUT_ENA — that
     * bit's reset value is 1 (enabled at ~2.43 V), so the hardware BOD fires
     * on any RF TX transient even when the app "disables" it via Kconfig.
     * Writing 0 to the register is the only way to fully disable it. */
    REG_WRITE(RTC_CNTL_BROWN_OUT_REG, 0);

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    /* Set timezone globally so localtime_r() always returns local time,
     * not UTC. Without this, context_builder stamps system prompt with UTC. */
    setenv("TZ", LANG_TIMEZONE, 1);
    tzset();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Langoustine - ESP32-S3 AI Assistant  ");
    ESP_LOGI(TAG, "========================================");

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    /* NOTE: esp_ota_mark_app_valid_cancel_rollback() is called after WiFi connects
     * so a firmware that crashes before network is up will auto-rollback on next boot. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* PSRAM / SRAM stats (logged before LittleFS to capture peak-free state) */
    ESP_LOGI(TAG, "Internal free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Min free heap:    %d bytes",
             (int)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Largest free blk: %d bytes",
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "PSRAM free:       %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "SRAM free:        %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* LED: init early so it shows booting state immediately */
    led_indicator_init();

    /* Log ring buffer: init early so I2C scan results are captured */
    log_buffer_init();

    /* I2C bus + OLED display: init early for boot status visibility */
#if LANG_OLED_ENABLED
    static i2c_master_bus_handle_t s_i2c_bus = NULL;
    static uint8_t  s_oled_addr = LANG_OLED_ADDR;
    static esp_err_t s_i2c_probe_3c = ESP_FAIL;
    static esp_err_t s_i2c_probe_3d = ESP_FAIL;
    static esp_err_t s_oled_ret = ESP_FAIL;
    {
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port     = I2C_NUM_0,
            .sda_io_num   = LANG_I2C_SDA,
            .scl_io_num   = LANG_I2C_SCL,
            .clk_source   = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t i2c_ret = i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
        if (i2c_ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C bus ready (SDA=%d, SCL=%d)", LANG_I2C_SDA, LANG_I2C_SCL);

            /* Full I2C bus scan: check all valid 7-bit addresses (0x08–0x77) */
            ESP_LOGI(TAG, "I2C bus scan starting...");
            int found_count = 0;
            for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                esp_err_t p = i2c_master_probe(s_i2c_bus, addr, 50);
                if (p == ESP_OK) {
                    ESP_LOGW(TAG, "I2C device found at 0x%02X", addr);
                    found_count++;
                }
            }
            ESP_LOGI(TAG, "I2C bus scan complete: %d device(s) found", found_count);

            s_i2c_probe_3c = i2c_master_probe(s_i2c_bus, 0x3C, 100);
            s_i2c_probe_3d = i2c_master_probe(s_i2c_bus, 0x3D, 100);

            if (s_i2c_probe_3c == ESP_OK) {
                s_oled_addr = 0x3C;
            } else if (s_i2c_probe_3d == ESP_OK) {
                s_oled_addr = 0x3D;
            }

            if (s_i2c_probe_3c == ESP_OK || s_i2c_probe_3d == ESP_OK) {
                s_oled_ret = oled_display_init(s_i2c_bus);
                ESP_LOGI(TAG, "OLED init (addr 0x%02X): %s",
                         s_oled_addr, esp_err_to_name(s_oled_ret));
            } else {
                ESP_LOGW(TAG, "No OLED found at 0x3C or 0x3D — display disabled");
            }

            if (s_oled_ret != ESP_OK) {
                ESP_LOGW(TAG, "OLED display init failed: %s (continuing without display)",
                         esp_err_to_name(s_oled_ret));
            }
        } else {
            ESP_LOGW(TAG, "I2C bus init failed: %s (OLED disabled)", esp_err_to_name(i2c_ret));
        }
    }
#endif

    ESP_ERROR_CHECK(init_littlefs());
    bootstrap_dirs();
    log_crash_if_needed();  /* Must run after LittleFS mount + dirs exist */
    bootstrap_defaults();

    /* Write I2C diagnostic results to LittleFS (now that it's mounted) */
#if LANG_OLED_ENABLED
    {
        FILE *f = fopen("/lfs/i2c_diag.txt", "w");
        if (f) {
            fprintf(f, "I2C SDA=GPIO%d SCL=GPIO%d\n", LANG_I2C_SDA, LANG_I2C_SCL);
            fprintf(f, "Probe 0x3C: %s\n", esp_err_to_name(s_i2c_probe_3c));
            fprintf(f, "Probe 0x3D: %s\n", esp_err_to_name(s_i2c_probe_3d));
            fprintf(f, "OLED init (addr 0x%02X): %s\n", s_oled_addr,
                esp_err_to_name(s_oled_ret));
            fclose(f);
            ESP_LOGI(TAG, "I2C diag written to /lfs/i2c_diag.txt");
        }
    }
#endif

    /* Audio pipeline (STT/TTS via Groq) — always required */
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(stt_client_init());
    ESP_ERROR_CHECK(tts_client_init());

#if LANG_I2S_AUDIO_ENABLED
    /* I2S speaker output via MAX98357A (GPIO 15/16/17, amp SD on GPIO42).
     * Also opens I2S RX for INMP441 mic on GPIO18.
     * Brownout mitigations in place: VIN→5V USB rail, WIFI_PS_NONE, BOD disabled. */
    {
        esp_err_t i2s_err = i2s_audio_init();
        if (i2s_err == ESP_OK) {
            ESP_LOGI(TAG, "I2S speaker ready (MAX98357A on GPIO%d/%d/%d, amp SD GPIO%d)",
                     LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DOUT, LANG_AMP_SD_GPIO);

            /* INMP441 mic: wake word ("Hi ESP") + PTT (BOOT button). */
            microphone_init();
            esp_err_t ww_err = wake_word_init();
            if (ww_err == ESP_OK) ww_err = wake_word_start();
            if (ww_err == ESP_OK) {
                ESP_LOGI(TAG, "INMP441 wake word + PTT ready (say \"Hi ESP\" or press BOOT)");
            } else {
                ESP_LOGW(TAG, "Wake word unavailable (%s) — INMP441 PTT-only",
                         esp_err_to_name(ww_err));
                microphone_start();
            }
        } else {
            ESP_LOGW(TAG, "I2S init failed: %s — speaker + mic disabled",
                     esp_err_to_name(i2s_err));
        }
    }
#endif

    /* UVC camera + UAC mic — both use the USB host stack.
     * UAC provides an alternative mic input if a USB audio device is connected.
     * INMP441 wake word (above) remains active regardless. */
    uvc_camera_init();
    uac_microphone_init();
    uvc_camera_start_host_task();

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    services_config_load();  /* Fill in API keys from SERVICES.md (NVS takes priority) */
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(rule_engine_init());
    ESP_ERROR_CHECK(agent_loop_init());
    ESP_ERROR_CHECK(telegram_bot_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi — or onboarding if no credentials stored */
    led_indicator_set(LED_WIFI);
    if (!wifi_onboard_has_credentials()) {
        ESP_LOGW(TAG, "No WiFi credentials found — starting onboarding captive portal");
        wifi_onboard_start();  /* blocks until user configures, then reboots */
        return;  /* unreachable — onboarding reboots */
    }

    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
            oled_display_set_ip(wifi_manager_get_ip());

            /* Advertise via mDNS */
            mdns_init();
            mdns_hostname_set("lango");
            mdns_instance_name_set("Langoustine AI Assistant");
            mdns_service_add(NULL, "_http", "_tcp", LANG_WS_PORT, NULL, 0);
            ESP_LOGI(TAG, "mDNS started: lango.local port %d", LANG_WS_PORT);

            /* Sync system clock via SNTP.
             * Set build timestamp immediately as fallback; SNTP will silently
             * overwrite the clock when a response arrives (async, no boot delay). */
            set_time_from_build();
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            esp_sntp_setservername(2, "time.cloudflare.com");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP started (syncing in background)");

            /* Outbound dispatch on Core 0 */
            ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                LANG_OUTBOUND_STACK, NULL,
                LANG_OUTBOUND_PRIO, NULL, 0) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            /* Start network-dependent services.
             * WS server first (small stack) before agent (large stack) to
             * avoid SRAM fragmentation preventing allocation of either. */
            ESP_ERROR_CHECK(ws_server_start());
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(telegram_bot_start());
            cron_service_start();
            heartbeat_start();
            rule_engine_start();

            /* All services up — mark OTA slot valid so rollback is cancelled.
             * Doing this here (after WiFi + services) means a firmware that
             * crashes during boot or before network is ready will automatically
             * roll back to the previous slot on the next boot attempt. */
            esp_ota_mark_app_valid_cancel_rollback();

            led_indicator_set(LED_READY);
            ESP_LOGI(TAG, "All services started!");

            /* Post-WiFi PSRAM stats */
            ESP_LOGI(TAG, "PSRAM free: %u bytes",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "SRAM free:  %u bytes",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

            /* Boot greeting removed — TTS HTTPS call + I2S playback during
             * boot was causing intermittent panics (SRAM pressure + brownout).
             * Use /api/say or the agent's say tool for on-demand speech. */
        } else {
            led_indicator_set(LED_ERROR);
            ESP_LOGW(TAG, "WiFi connection timeout. Check LANG_SECRET_WIFI_SSID.");
        }
    } else {
        led_indicator_set(LED_ERROR);
        ESP_LOGW(TAG, "No WiFi credentials. Set LANG_SECRET_WIFI_SSID.");
    }

    ESP_LOGI(TAG, "Langoustine ready. Type 'help' for CLI commands.");
}
