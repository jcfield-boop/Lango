#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include <time.h>
#include <sys/time.h>

#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
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
#include "audio/i2s_audio.h"
#include "audio/audio_pipeline.h"
#include "audio/stt_client.h"
#include "audio/tts_client.h"
#include "audio/microphone.h"
#include "audio/wake_word.h"
#include "camera/uvc_camera.h"
#include "led/led_indicator.h"
#include "mdns.h"

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
        mimi_msg_t msg;
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
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Langoustine - ESP32-S3 AI Assistant  ");
    ESP_LOGI(TAG, "========================================");

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
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

    ESP_ERROR_CHECK(init_littlefs());
    bootstrap_dirs();
    bootstrap_defaults();

#if LANG_I2S_AUDIO_ENABLED
    ESP_ERROR_CHECK(i2s_audio_init());
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(stt_client_init());
    ESP_ERROR_CHECK(tts_client_init());
#endif

    /* UVC camera — best-effort; continues if no webcam is connected */
    uvc_camera_init();

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(rule_engine_init());
    ESP_ERROR_CHECK(agent_loop_init());
    ESP_ERROR_CHECK(telegram_bot_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    led_indicator_set(LED_WIFI);
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Advertise via mDNS */
            mdns_init();
            mdns_hostname_set("langoustine");
            mdns_instance_name_set("Langoustine AI Assistant");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "mDNS started: langoustine.local");

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

            /* Start network-dependent services */
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(telegram_bot_start());
            cron_service_start();
            heartbeat_start();
            rule_engine_start();
            ESP_ERROR_CHECK(ws_server_start());

            /* Local voice: PTT button + wake word.
             * microphone_init() configures GPIO 0 for both paths.
             * wake_word_start() runs the AFE+WakeNet task and handles PTT internally;
             * microphone_start() is not called when wake word is active. */
#if LANG_I2S_AUDIO_ENABLED
            ESP_ERROR_CHECK(microphone_init());
            if (wake_word_start() != ESP_OK) {
                /* Wake word unavailable (no esp-sr model?) — fall back to PTT only */
                ESP_LOGW(TAG, "Wake word unavailable, using PTT only");
                ESP_ERROR_CHECK(microphone_start());
            }
#endif

            led_indicator_set(LED_READY);
            ESP_LOGI(TAG, "All services started!");

            /* Post-WiFi PSRAM stats */
            ESP_LOGI(TAG, "PSRAM free: %u bytes",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "SRAM free:  %u bytes",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
