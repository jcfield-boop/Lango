#pragma once

/* Langoustine Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("langoustine_secrets.h")
#include "langoustine_secrets.h"
#endif

#ifndef LANG_SECRET_WIFI_SSID
#define LANG_SECRET_WIFI_SSID       ""
#endif
#ifndef LANG_SECRET_WIFI_PASS
#define LANG_SECRET_WIFI_PASS       ""
#endif
#ifndef LANG_SECRET_API_KEY
#define LANG_SECRET_API_KEY         ""
#endif
#ifndef LANG_SECRET_MODEL
#define LANG_SECRET_MODEL           ""
#endif
#ifndef LANG_SECRET_MODEL_PROVIDER
#define LANG_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef LANG_SECRET_PROXY_HOST
#define LANG_SECRET_PROXY_HOST      ""
#endif
#ifndef LANG_SECRET_PROXY_PORT
#define LANG_SECRET_PROXY_PORT      ""
#endif
#ifndef LANG_SECRET_SEARCH_KEY
#define LANG_SECRET_SEARCH_KEY      ""
#endif
#ifndef LANG_SECRET_TG_TOKEN
#define LANG_SECRET_TG_TOKEN        ""
#endif
#ifndef LANG_SECRET_HTTP_TOKEN
#define LANG_SECRET_HTTP_TOKEN      ""
#endif

/* WiFi */
#define LANG_WIFI_MAX_RETRY          10
#define LANG_WIFI_RETRY_BASE_MS      1000
#define LANG_WIFI_RETRY_MAX_MS       30000

/* Agent Loop */
#define LANG_AGENT_STACK             (24 * 1024)
#define LANG_AGENT_PRIO              6
#define LANG_AGENT_CORE              1
#define LANG_AGENT_MAX_HISTORY       30
#define LANG_AGENT_MAX_TOOL_ITER     10
#define LANG_MAX_TOOL_CALLS          4
#define LANG_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define LANG_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define LANG_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define LANG_LLM_PROVIDER_DEFAULT    "anthropic"
#define LANG_LLM_MAX_TOKENS          1000
#define LANG_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define LANG_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define LANG_OPENROUTER_API_URL      "https://openrouter.ai/api/v1/chat/completions"
#define LANG_OPENROUTER_REFERER      "https://github.com/memovai/langoustine"
#define LANG_OPENROUTER_TITLE        "Langoustine"
#define LANG_LLM_API_VERSION         "2023-06-01"
#define LANG_LLM_STREAM_BUF_SIZE     (64 * 1024)  /* PSRAM — larger for S3 */
#define LANG_LLM_LOG_VERBOSE_PAYLOAD 0
#define LANG_LLM_LOG_PREVIEW_BYTES   160

/* PSRAM Buffer Sizes */
#define LANG_AUDIO_RING_SIZE         (256 * 1024) /* 256KB audio ring buffer in PSRAM */
#define LANG_AUDIO_MAX_UPLOAD_BYTES  (512 * 1024) /* max audio upload size */
#define LANG_TTS_BUF_SIZE            (512 * 1024) /* 512KB per TTS cache entry */
#define LANG_TTS_CACHE_MAX           4            /* max concurrent TTS cache entries */
#define LANG_CONTEXT_BUF_SIZE        (32 * 1024)  /* 32KB context buffer in PSRAM */

/* Core Assignment */
#define LANG_STT_CORE                1
#define LANG_TTS_CORE                1

/* Rate Limiting */
#define LANG_RATE_LIMIT_RPM          10

/* STT defaults (Groq Whisper-compatible) */
#define LANG_DEFAULT_STT_ENDPOINT    "https://api.groq.com/openai/v1/audio/transcriptions"
#define LANG_DEFAULT_STT_MODEL       "whisper-large-v3-turbo"

/* TTS defaults (Groq Orpheus) */
#define LANG_DEFAULT_TTS_ENDPOINT    "https://api.groq.com/openai/v1/audio/speech"
#define LANG_DEFAULT_TTS_MODEL       "canopylabs/orpheus-v1-english"
#define LANG_DEFAULT_TTS_VOICE       "tara"

/* RGB Status LED — WS2812/NeoPixel on dev board via RMT */
#define LANG_LED_GPIO    38   /* GPIO38 = onboard NeoPixel on ESP32-S3-DevKitC-1 */

/* I2S Audio — shared BCLK/LRCLK for speaker + mic */
#define LANG_I2S_BCLK    15   /* to MAX98357A BCLK + INMP441 SCK  */
#define LANG_I2S_LRCLK   16   /* to MAX98357A LRC  + INMP441 WS   */
#define LANG_I2S_DOUT    17   /* to MAX98357A DIN                  */
#define LANG_I2S_DIN     18   /* from INMP441 SD                   */

/* Software volume scale: 0–256, where 256 = 100% (0 dB), 128 = 50% (−6 dB).
 * Lower values reduce peak amp current and prevent brownouts on weak PSUs. */
#ifndef LANG_AUDIO_VOLUME
#define LANG_AUDIO_VOLUME  128
#endif

/* MAX98357A SD (shutdown) pin — optional amp power gating.
 * Wire SD pin to this GPIO; the driver pulls it high before playback and low
 * after, eliminating idle current draw and preventing turn-on pop.
 * Set to -1 (default) if the SD pin is floating / not connected to a GPIO.
 * Example: wire SD to GPIO 45 and set LANG_AMP_SD_GPIO 45 here. */
#ifndef LANG_AMP_SD_GPIO
#define LANG_AMP_SD_GPIO  42
#endif

/* PTT button — BOOT button, active low, internal pull-up */
#define LANG_PTT_GPIO              0

/* Microphone capture parameters (INMP441, 16kHz 16-bit mono) */
#define LANG_MIC_SAMPLE_RATE    16000
#define LANG_MIC_BITS              16
#define LANG_MIC_READ_CHUNK_BYTES 512   /* ~16 ms of audio per I2S read */
#define LANG_MIC_STACK_SIZE      4096
#define LANG_MIC_TASK_PRIO          5

/* I2C bus (camera SCCB + future PCA9685 servos) */
#define LANG_I2C_SDA      9
#define LANG_I2C_SCL     10

/* Enable local speaker playback via MAX98357A after TTS generation */
#define LANG_I2S_AUDIO_ENABLED  0

/* Message Bus */
#define LANG_BUS_QUEUE_LEN           16
#define LANG_OUTBOUND_STACK          (6 * 1024)
#define LANG_OUTBOUND_PRIO           5
#define LANG_OUTBOUND_CORE           0

/* LittleFS paths */
#define LANG_LFS_BASE                "/lfs"
#define LANG_LFS_CONFIG_DIR          "/lfs/config"
#define LANG_LFS_MEMORY_DIR          "/lfs/memory"
#define LANG_LFS_SESSION_DIR         "/lfs/sessions"
#define LANG_LFS_SKILLS_DIR          "/lfs/skills"
#define LANG_LFS_TTS_CACHE_DIR       "/lfs/tts"
#define LANG_MEMORY_FILE             "/lfs/memory/MEMORY.md"
#define LANG_MEMORY_MAX_BYTES        (6 * 1024)
#define LANG_SOUL_FILE               "/lfs/config/SOUL.md"
#define LANG_USER_FILE               "/lfs/config/USER.md"
#define LANG_SESSION_MAX_MSGS        30
#define LANG_SESSION_HISTORY_MAX_BYTES (12 * 1024)

/* Cron / Heartbeat */
#define LANG_CRON_FILE               "/lfs/cron.json"
#define LANG_CRON_MAX_JOBS           16
#define LANG_CRON_CHECK_INTERVAL_MS  (10 * 1000)
#define LANG_HEARTBEAT_FILE          "/lfs/HEARTBEAT.md"
#define LANG_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* UVC Camera (USB webcam on GPIO 19/20) */
#define LANG_CAMERA_BUF_SIZE            (64 * 1024) /* PSRAM: max JPEG frame */
#define LANG_CAMERA_CAPTURE_TIMEOUT_MS  3000
#define LANG_CAMERA_CAPTURE_DIR         "/lfs/captures"
#define LANG_CAMERA_CAPTURE_PATH        "/lfs/captures/latest.jpg"
#define LANG_VISION_MAX_TOKENS          512

/* Web Search */
#define LANG_TAVILY_BUF_SIZE         (16 * 1024)

/* Skills */
#define LANG_SKILLS_PREFIX           "/lfs/skills/"

/* WebSocket Gateway */
#define LANG_WS_PORT                 80
#define LANG_WSS_PORT                443
#define LANG_WS_MAX_CLIENTS          4
#define LANG_WS_MAX_AUDIO_FRAME      (32 * 1024)

/* Serial CLI */
#define LANG_CLI_STACK               (4 * 1024)
#define LANG_CLI_PRIO                3
#define LANG_CLI_CORE                0

/* NVS Namespaces */
#define LANG_NVS_WIFI                "wifi_config"
#define LANG_NVS_LLM                 "llm_config"
#define LANG_NVS_PROXY               "proxy_config"
#define LANG_NVS_SEARCH              "search_config"
#define LANG_NVS_STT                 "stt_config"
#define LANG_NVS_TTS                 "tts_config"
#define LANG_NVS_AUDIO               "audio_config"

/* NVS Keys */
#define LANG_NVS_KEY_SSID            "ssid"
#define LANG_NVS_KEY_PASS            "password"
#define LANG_NVS_KEY_API_KEY         "api_key"
#define LANG_NVS_KEY_MODEL           "model"
#define LANG_NVS_KEY_PROVIDER        "provider"
#define LANG_NVS_KEY_PROXY_HOST      "host"
#define LANG_NVS_KEY_PROXY_PORT      "port"
#define LANG_NVS_KEY_VOICE           "voice"
#define LANG_NVS_KEY_ENDPOINT        "endpoint"
#define LANG_NVS_KEY_VOLUME          "volume"

/* Telegram Bot */
#define LANG_TG_POLL_TIMEOUT_S       30
#define LANG_TG_MAX_MSG_LEN          4096
#define LANG_TG_POLL_STACK           (8 * 1024)
#define LANG_TG_POLL_PRIO            5
#define LANG_TG_POLL_CORE            0
#define LANG_NVS_TG                  "tg_config"
#define LANG_NVS_KEY_TG_TOKEN        "bot_token"

/* Channel Names */
#define LANG_CHAN_WEBSOCKET           "websocket"
#define LANG_CHAN_SYSTEM              "system"
#define LANG_CHAN_TELEGRAM            "telegram"

/* Compatibility aliases for modules not yet fully renamed */
#ifndef MIMI_CHAN_WEBSOCKET
#define MIMI_CHAN_WEBSOCKET           LANG_CHAN_WEBSOCKET
#endif
#ifndef MIMI_CHAN_SYSTEM
#define MIMI_CHAN_SYSTEM              LANG_CHAN_SYSTEM
#endif
#ifndef MIMI_CHAN_TELEGRAM
#define MIMI_CHAN_TELEGRAM            LANG_CHAN_TELEGRAM
#endif
#define MIMI_BUS_QUEUE_LEN           LANG_BUS_QUEUE_LEN
#define MIMI_AGENT_MAX_HISTORY       LANG_AGENT_MAX_HISTORY
#define MIMI_MAX_TOOL_CALLS          LANG_MAX_TOOL_CALLS
#define MIMI_TIMEZONE                LANG_TIMEZONE
#define MIMI_SESSION_MAX_MSGS        LANG_SESSION_MAX_MSGS
#define MIMI_SESSION_HISTORY_MAX_BYTES LANG_SESSION_HISTORY_MAX_BYTES
#define MIMI_AGENT_MAX_TOOL_ITER     LANG_AGENT_MAX_TOOL_ITER
#define MIMI_LLM_MAX_TOKENS          LANG_LLM_MAX_TOKENS
#define MIMI_LLM_API_URL             LANG_LLM_API_URL
#define MIMI_OPENAI_API_URL          LANG_OPENAI_API_URL
#define MIMI_OPENROUTER_API_URL      LANG_OPENROUTER_API_URL
#define MIMI_OPENROUTER_REFERER      LANG_OPENROUTER_REFERER
#define MIMI_OPENROUTER_TITLE        LANG_OPENROUTER_TITLE
#define MIMI_LLM_API_VERSION         LANG_LLM_API_VERSION
#define MIMI_LLM_STREAM_BUF_SIZE     LANG_LLM_STREAM_BUF_SIZE
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD LANG_LLM_LOG_VERBOSE_PAYLOAD
#define MIMI_LLM_LOG_PREVIEW_BYTES   LANG_LLM_LOG_PREVIEW_BYTES
#define MIMI_LLM_DEFAULT_MODEL       LANG_LLM_DEFAULT_MODEL
#define MIMI_LLM_PROVIDER_DEFAULT    LANG_LLM_PROVIDER_DEFAULT
#define MIMI_WIFI_MAX_RETRY          LANG_WIFI_MAX_RETRY
#define MIMI_WIFI_RETRY_BASE_MS      LANG_WIFI_RETRY_BASE_MS
#define MIMI_WIFI_RETRY_MAX_MS       LANG_WIFI_RETRY_MAX_MS
#define MIMI_SPIFFS_BASE             LANG_LFS_BASE
#define MIMI_SPIFFS_CONFIG_DIR       LANG_LFS_CONFIG_DIR
#define MIMI_SPIFFS_MEMORY_DIR       LANG_LFS_MEMORY_DIR
#define MIMI_SPIFFS_SESSION_DIR      LANG_LFS_SESSION_DIR
#define MIMI_MEMORY_FILE             LANG_MEMORY_FILE
#define MIMI_MEMORY_MAX_BYTES        LANG_MEMORY_MAX_BYTES
#define MIMI_SOUL_FILE               LANG_SOUL_FILE
#define MIMI_USER_FILE               LANG_USER_FILE
#define MIMI_CONTEXT_BUF_SIZE        LANG_CONTEXT_BUF_SIZE
#define MIMI_CRON_FILE               LANG_CRON_FILE
#define MIMI_CRON_MAX_JOBS           LANG_CRON_MAX_JOBS
#define MIMI_CRON_CHECK_INTERVAL_MS  LANG_CRON_CHECK_INTERVAL_MS
#define MIMI_HEARTBEAT_FILE          LANG_HEARTBEAT_FILE
#define MIMI_HEARTBEAT_INTERVAL_MS   LANG_HEARTBEAT_INTERVAL_MS
#define MIMI_TAVILY_BUF_SIZE         LANG_TAVILY_BUF_SIZE
#define MIMI_SKILLS_PREFIX           LANG_SKILLS_PREFIX
#define MIMI_WS_PORT                 LANG_WS_PORT
#define MIMI_WS_MAX_CLIENTS          LANG_WS_MAX_CLIENTS
#define MIMI_NVS_WIFI                LANG_NVS_WIFI
#define MIMI_NVS_LLM                 LANG_NVS_LLM
#define MIMI_NVS_PROXY               LANG_NVS_PROXY
#define MIMI_NVS_SEARCH              LANG_NVS_SEARCH
#define MIMI_NVS_KEY_SSID            LANG_NVS_KEY_SSID
#define MIMI_NVS_KEY_PASS            LANG_NVS_KEY_PASS
#define MIMI_NVS_KEY_API_KEY         LANG_NVS_KEY_API_KEY
#define MIMI_NVS_KEY_MODEL           LANG_NVS_KEY_MODEL
#define MIMI_NVS_KEY_PROVIDER        LANG_NVS_KEY_PROVIDER
#define MIMI_NVS_KEY_PROXY_HOST      LANG_NVS_KEY_PROXY_HOST
#define MIMI_NVS_KEY_PROXY_PORT      LANG_NVS_KEY_PROXY_PORT
#define MIMI_SECRET_WIFI_SSID        LANG_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_PASS        LANG_SECRET_WIFI_PASS
#define MIMI_SECRET_API_KEY          LANG_SECRET_API_KEY
#define MIMI_SECRET_MODEL            LANG_SECRET_MODEL
#define MIMI_SECRET_MODEL_PROVIDER   LANG_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_PROXY_HOST       LANG_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_PORT       LANG_SECRET_PROXY_PORT
#define MIMI_SECRET_SEARCH_KEY       LANG_SECRET_SEARCH_KEY
#define MIMI_SECRET_TG_TOKEN         LANG_SECRET_TG_TOKEN
#define MIMI_TG_POLL_TIMEOUT_S       LANG_TG_POLL_TIMEOUT_S
#define MIMI_TG_MAX_MSG_LEN          LANG_TG_MAX_MSG_LEN
#define MIMI_TG_POLL_STACK           LANG_TG_POLL_STACK
#define MIMI_TG_POLL_PRIO            LANG_TG_POLL_PRIO
#define MIMI_NVS_TG                  LANG_NVS_TG
#define MIMI_NVS_KEY_TG_TOKEN        LANG_NVS_KEY_TG_TOKEN
