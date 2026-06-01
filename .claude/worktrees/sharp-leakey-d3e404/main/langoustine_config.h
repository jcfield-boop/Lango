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
#define LANG_AGENT_STACK             (32 * 1024)
#define LANG_AGENT_PRIO              6
#define LANG_AGENT_CORE              1
#define LANG_AGENT_MAX_HISTORY       60
#define LANG_AGENT_MAX_TOOL_ITER     10
#define LANG_MAX_TOOL_CALLS          2
#define LANG_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define LANG_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define LANG_LLM_DEFAULT_MODEL       "openrouter/auto"
#define LANG_LLM_PROVIDER_DEFAULT    "openrouter"
#define LANG_LLM_MAX_TOKENS          4096
#define LANG_APFEL_MAX_TOKENS        1024  /* conservative for Apple FM's 4K context */
#define LANG_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define LANG_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define LANG_OPENROUTER_API_URL      "https://openrouter.ai/api/v1/chat/completions"
#define LANG_OPENROUTER_REFERER      "https://github.com/memovai/langoustine"
#define LANG_OPENROUTER_TITLE        "Langoustine"
#define LANG_LLM_API_VERSION         "2024-10-22"
#define LANG_LLM_STREAM_BUF_SIZE     (64 * 1024)  /* PSRAM — larger for S3 */
#define LANG_LLM_LOG_VERBOSE_PAYLOAD 0
#define LANG_LLM_LOG_PREVIEW_BYTES   160

/* PSRAM Buffer Sizes */
#define LANG_AUDIO_RING_SIZE         (256 * 1024) /* 256KB audio ring buffer in PSRAM */
#define LANG_AUDIO_MAX_UPLOAD_BYTES  (512 * 1024) /* max audio upload size */
#define LANG_TTS_BUF_SIZE            (512 * 1024) /* 512KB per TTS cache entry */
#define LANG_TTS_CACHE_MAX           4            /* max concurrent TTS cache entries */
#define LANG_CONTEXT_BUF_SIZE        (64 * 1024)  /* 64KB context buffer in PSRAM */

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
#define LANG_DEFAULT_TTS_VOICE       "autumn"  /* valid Groq Orpheus voices: autumn diana hannah austin daniel troy */

/* RGB Status LED — WS2812/NeoPixel on dev board via RMT */
#define LANG_LED_GPIO    38   /* GPIO38 = onboard NeoPixel on ESP32-S3-DevKitC-1 */

/* I2S Audio — full-duplex on I2S_NUM_0 (shared bus for speaker + mic).
 * MAX98357A and INMP441 share BCLK/WS on GPIO 3/4.
 * TX is master (generates clocks), RX is slave (sig_loopback).
 * Silence pump keeps TX DMA active so BCLK runs continuously for mic.
 * Amp gated by GPIO 42 — silence writes produce no audible output. */
#define LANG_I2S_BCLK     3   /* shared: MAX98357A BCLK + INMP441 SCK */
#define LANG_I2S_LRCLK    4   /* shared: MAX98357A LRC  + INMP441 WS  */
#define LANG_I2S_DOUT     6   /* TX: to MAX98357A DIN                 */
#define LANG_I2S_DIN      5   /* RX: from INMP441 SD                  */

/* Software volume scale: 0–256, where 256 = 100% (0 dB), 128 = 50% (−6 dB).
 * Lower values reduce peak amp current and prevent brownouts on weak PSUs. */
#ifndef LANG_AUDIO_VOLUME
#define LANG_AUDIO_VOLUME  255
#endif

/* MAX98357A SD (shutdown) pin — optional amp power gating.
 * Wire SD pin to this GPIO; the driver pulls it high before playback and low
 * after, eliminating idle current draw and preventing turn-on pop.
 * Set to -1 (default) if the SD pin is floating / not connected to a GPIO.
 * Example: wire SD to GPIO 45 and set LANG_AMP_SD_GPIO 45 here. */
#ifndef LANG_AMP_SD_GPIO
#define LANG_AMP_SD_GPIO  42   /* MAX98357A shutdown: HIGH=on, LOW=off */
#endif

/* I2S DMA buffer tuning for gapless speaker playback.
 * Each DMA buffer holds dma_frame_num frames of (data_bit_width/8 * slots) bytes.
 * At 24kHz mono 16-bit: 480 frames × 2 bytes = 960 B/buf, 8 bufs = 7.7KB TX.
 * Provides ~160ms buffering at 24kHz — survives WiFi TX bursts (10-50ms). */
#define LANG_I2S_DMA_DESC_NUM    8    /* number of DMA descriptors (was 6 default) */
#define LANG_I2S_DMA_FRAME_NUM   480  /* frames per DMA buffer (was 240 default) */

/* Software fade duration (ms) applied at start/end of WAV playback.
 * Eliminates pop/click from abrupt amp enable/disable transitions. */
#define LANG_I2S_FADE_MS         15

/* PTT button — BOOT button, active low, internal pull-up */
#define LANG_PTT_GPIO              0

/* Microphone capture parameters (INMP441, 16kHz 16-bit mono) */
#define LANG_MIC_SAMPLE_RATE    16000
#define LANG_MIC_BITS              16
#define LANG_MIC_READ_CHUNK_BYTES 512   /* ~16 ms of audio per I2S read */
#define LANG_MIC_STACK_SIZE      12288  /* 12KB: UAC ctrl xfers need headroom, 8KB caused StoreProhibited */
#define LANG_MIC_TASK_PRIO          5

/* I2C bus (OLED display + BME280 sensor + future peripherals) */
#define LANG_I2C_SDA        9
#define LANG_I2C_SCL       10
#define LANG_I2C_FREQ_HZ   100000   /* 100 kHz standard mode — safer with dupont wires */

/* SSD1306 OLED display */
#define LANG_OLED_ADDR      0x3C    /* default I2C address */
#define LANG_OLED_WIDTH     128
#define LANG_OLED_HEIGHT    64
#define LANG_OLED_ENABLED   1

/* Enable local speaker playback via MAX98357A after TTS generation */
#define LANG_I2S_AUDIO_ENABLED  1

/* Message Bus */
#define LANG_BUS_QUEUE_LEN           16
#define LANG_OUTBOUND_STACK          (8 * 1024)
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
#define LANG_MEMORY_MAX_BYTES        (16 * 1024)
#define LANG_SOUL_FILE               "/lfs/config/SOUL.md"
#define LANG_USER_FILE               "/lfs/config/USER.md"
#define LANG_SESSION_MAX_MSGS        200
#define LANG_SESSION_HISTORY_MAX_BYTES (64 * 1024)

/* Cron / Heartbeat */
#define LANG_CRON_FILE               "/lfs/cron.json"
#define LANG_CRON_MAX_JOBS           16
#define LANG_CRON_CHECK_INTERVAL_MS  (10 * 1000)
#define LANG_HEARTBEAT_FILE          "/lfs/HEARTBEAT.md"
#define LANG_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* Soak log — firmware-enforced cap on /lfs/memory/soak.md to prevent
 * unbounded growth when the LLM forgets the nightly truncate directive.
 * 96 lines ≈ 48h of [30m] heartbeat entries. */
#define LANG_SOAK_FILE               "/lfs/memory/soak.md"
#define LANG_SOAK_MAX_LINES          96

/* Heartbeat diagnostic warnings — emit a monitor/OLED alert when a single
 * agent turn balloons (indicates ReAct context accumulation / tool-loop). */
#define LANG_HEARTBEAT_WARN_ITERS      5
#define LANG_HEARTBEAT_WARN_INPUT_TOK  30000

/* Pico Phase A — rolling context window for long tool chains.
 * Keep original user message + this many (assistant, tool_result) pairs.
 * Older pairs are dropped after each iteration to keep the in-flight
 * messages[] array from growing unboundedly on multi-step cron/briefing
 * tasks (6-8 tool calls) that would otherwise balloon to 40-50 KB and
 * either OOM the agent task or hit the 5-min soft timeout. */
#define LANG_AGENT_CONTEXT_TRIM_ITERS  4

/* UVC Camera (USB webcam on GPIO 19/20) */
#define LANG_CAMERA_BUF_SIZE            (64 * 1024) /* PSRAM: max JPEG frame */
#define LANG_CAMERA_WARMUP_MS           3000   /* time-based AEC/AGC warmup before capture */
#define LANG_CAMERA_CAPTURE_TIMEOUT_MS  (LANG_CAMERA_WARMUP_MS + 5000) /* warmup + frame wait */
#define LANG_CAMERA_CAPTURE_DIR         "/lfs/captures"
#define LANG_CAMERA_CAPTURE_PATH        "/lfs/captures/latest.jpg"
#define LANG_VISION_MAX_TOKENS          512

/* Web Search */
#define LANG_TAVILY_BUF_SIZE         (32 * 1024)

/* Skills */
#define LANG_SKILLS_PREFIX           "/lfs/skills/"

/* WebSocket Gateway */
#define LANG_WS_PORT                 80
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

/* Legacy MIMI_* aliases removed — all code now uses LANG_* directly */
