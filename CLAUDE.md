# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires ESP-IDF 6.0+ sourced in the shell. `sdkconfig` is gitignored — regenerate after cloning:

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # generates sdkconfig from sdkconfig.defaults.esp32s3
idf.py build                # produces build/langoustine.bin + build/littlefs.bin
idf.py -p /dev/tty.usbserial-* flash monitor
```

The LittleFS partition image (`littlefs.bin`) is built automatically from `littlefs_data/` and flashed alongside the firmware. First flash must include `--flash-size 32MB`.

Full flash command (after `idf.py build`):
```bash
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 32MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x1a000 build/ota_data_initial.bin \
  0x20000 build/langoustine.bin \
  0x830000 build/littlefs.bin
```

## Architecture

Langoustine is an ESP32-S3 AI assistant (32MB flash, 16MB OPI PSRAM) with voice and Telegram interfaces.

### Core Assignment

| Core | Tasks |
|------|-------|
| **0** | WiFi/lwIP, httpd (HTTP+WebSocket), outbound dispatch, Telegram polling, serial CLI |
| **1** | Agent loop (LLM), STT task, TTS task, cron service |

AI pipeline tasks are pinned to Core 1 via `xTaskCreatePinnedToCore`. FreeRTOS queues (`message_bus`) bridge the cores for text; `xTaskNotifyGive/Take` signals the STT task when audio is committed.

### Message Flow

```
[WebSocket client]──text/audio──▶ ws_server.c ──▶ message_bus (inbound)
[Telegram]──────────────────────▶ telegram_bot.c ─▶ message_bus (inbound)
[Cron / heartbeat]──────────────────────────────▶ message_bus (inbound)
                                                      │
                                              agent_loop.c (Core 1)
                                              LLM API + tool calls
                                                      │
                                         message_bus (outbound)
                                                      │
                              outbound_dispatch_task (Core 0)
                             ┌────────────────────────┴──────────────────────┐
                    ws_server_send()                          telegram_send_message()
```

Agent identifies the originating channel from `msg.channel` ("websocket", "telegram", "system") and routes the response accordingly. Telegram messages additionally use the placeholder/edit pattern (`telegram_send_get_id` before the LLM call, `telegram_edit_message` after).

### Audio Pipeline (voice mode)

1. Browser sends `audio_start` JSON → `audio_ring_open(chat_id, mime)`
2. Browser sends binary WebSocket frames → `audio_ring_append()` into 256KB PSRAM ring buffer
3. Browser sends `audio_end` → `audio_ring_commit()` signals the STT task on Core 1
4. STT task POSTs to Groq Whisper API → pushes transcript as inbound message
5. Agent processes transcript → generates response → calls `tts_generate()`
6. TTS client POSTs to Groq PlayAI API → WAV cached in PSRAM (up to 4 entries, 512KB each, 5-min TTL)
7. If `LANG_I2S_AUDIO_ENABLED`: WAV is played immediately via MAX98357A on GPIO 3/4/6
8. `{"type":"message","tts_id":"<8hex>"}` sent to browser → browser fetches `GET /tts/<id>`

## Configuration & Secrets

### Hierarchy (highest → lowest priority)
1. **NVS** — set at runtime via CLI or `/api/config`. Survives reboots.
2. **Build-time** — `main/langoustine_secrets.h` (gitignored, copy from `.example`).
3. **Defaults** — hardcoded empty strings in `main/langoustine_config.h`.

### Setting secrets at runtime (no rebuild needed)
```
lango> set_wifi <ssid> <password>
lango> set_api_key <anthropic-or-openai-key>
lango> set_tg_token <telegram-bot-token>
lango> stt_key <groq-key>
lango> tts_key <groq-key>
lango> config_show          # shows all values with source (NVS/build)
lango> config_reset         # wipes all NVS overrides
```

All constants live in `main/langoustine_config.h` under the `LANG_` prefix. Verbatim-copied modules that haven't been fully renamed use `MIMI_` aliases defined in the same file and in `main/mimi_config.h` (a one-line stub that includes `langoustine_config.h`).

### NVS Namespaces
`wifi_config`, `llm_config`, `tg_config`, `stt_config`, `tts_config`, `proxy_config`, `search_config`

## PSRAM Allocation Rule

Use `ps_malloc` / `ps_calloc` / `ps_realloc` (from `main/memory/psram_alloc.h`) for **all buffers > 4KB**: audio ring, LLM stream buffer, TTS cache entries, cJSON parse trees, context builder, multipart HTTP bodies.

Use `int_malloc` / `int_calloc` for FreeRTOS objects (queues, semaphores, mutexes) and DMA buffers — these must stay in SRAM.

Plain `malloc` routes to SRAM for small allocations (≤4KB) and PSRAM for larger ones per `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`.

## LittleFS Layout

Mounted at `/lfs` (~24MB partition). Static content in `littlefs_data/` is baked into `littlefs.bin` at build time.

```
/lfs/config/    SOUL.md (system prompt), USER.md (user profile), SERVICES.md
/lfs/memory/    MEMORY.md (long-term episodic memory, 6KB max)
/lfs/sessions/  <chat_id>.json (per-user history, 15 msgs, 6KB max)
/lfs/skills/    *.md skill definitions loaded by the agent
/lfs/tts/       (runtime TTS cache IDs, audio served from PSRAM)
/lfs/console/   index.html (voice UI), dev.html (dev console)
/lfs/cron.json  Scheduled job definitions
/lfs/HEARTBEAT.md  Periodic task checklist (checked every 30 min)
```

Edit `SOUL.md` to change the assistant's personality. Edit `USER.md` to set the user's name/timezone.

## Hardware — Pin Map

Board: **ESP32-S3-WROOM-2-N32R16V** (PCB antenna, 32MB Octal Flash, 16MB Octal PSRAM)
Available GPIOs: 0–21, 38–48. GPIO 22–37 used internally for Flash/PSRAM.

| GPIO | Function | Connected to |
|------|----------|-------------|
| 3 | I2S_BCLK (TX) | MAX98357A BCLK (speaker) |
| 4 | I2S_LRCLK (TX) | MAX98357A LRC (speaker) |
| 6 | I2S_DOUT (TX) | MAX98357A DIN (speaker) |
| 7 | I2S_RX_BCLK | INMP441 SCK (mic, separate I2S port) |
| 8 | I2S_RX_LRCLK | INMP441 WS (mic, separate I2S port) |
| 5 | I2S_DIN (RX) | INMP441 SD (microphone) |
| 9 | I2C_SDA | Camera SCCB + PCA9685 |
| 10 | I2C_SCL | Camera SCCB + PCA9685 |
| 1–2, 11–18, 21 | Cam D0–D7 | OV2640 parallel data (future) |
| 38–41 | Cam XCLK/PCLK/VSYNC/HREF | OV2640 timing (future) |
| 42 | AMP_SD | MAX98357A SD (amp shutdown/enable) |
| 43 | UART0 TX | Serial CLI |
| 44 | UART0 RX | Serial CLI |

Constants in `main/langoustine_config.h` under `LANG_I2S_*` and `LANG_I2C_*`.

### Wiring Guide

**MAX98357A breakout → ESP32-S3**
```
VIN   → 5V  (USB rail — keeps amp current off the 3.3V/ESP32 rail entirely)
GND   → GND
DIN   → GPIO6
BCLK  → GPIO3
LRC   → GPIO4
SD    → GPIO42  (firmware-controlled: high=enabled, low=shutdown; eliminates idle hiss)
GAIN  → GND  (3 dB gain; floating = 12 dB default)
```

**Speaker → MAX98357A**
```
Red wire  → + terminal
Black wire→ − terminal
⚠ Do NOT connect speaker ground to system GND (bridge-tied output)
```

**INMP441 breakout → ESP32-S3**
```
VDD  → 3.3V  (max 3.6V — never 5V)
GND  → GND
SD   → GPIO5
SCK  → GPIO7   (dedicated RX I2S port — NOT shared with speaker)
WS   → GPIO8   (dedicated RX I2S port — NOT shared with speaker)
L/R  → GND  (left channel)
```

## Adding a New Tool

1. Create `main/tools/tool_<name>.c` and `.h` following the pattern of any existing tool (e.g. `tool_get_time.c`): implement `tool_<name>_execute(input_json, output, output_size)`.
2. Add `"tools/tool_<name>.c"` to `main/CMakeLists.txt` SRCS.
3. Register in `main/tools/tool_registry.c`: add schema JSON and a call to `tool_registry_register()`.

## Key Files

| File | Purpose |
|------|---------|
| `main/langoustine.c` | App entry point: init sequence, outbound dispatch task |
| `main/langoustine_config.h` | All `LANG_*` constants and `MIMI_*` compat aliases |
| `main/langoustine_secrets.h.example` | Template for build-time secrets |
| `main/agent/agent_loop.c` | Core AI ReAct loop (Core 1) |
| `main/agent/context_builder.c` | Assembles system prompt from SOUL/USER/MEMORY/skills |
| `main/gateway/ws_server.c` | HTTP+WebSocket server, all REST endpoints |
| `main/audio/audio_pipeline.c` | PSRAM ring buffer + STT task coordination |
| `main/audio/stt_client.c` | Groq Whisper multipart POST |
| `main/audio/tts_client.c` | Groq PlayAI POST + PSRAM cache; local I2S playback |
| `main/audio/i2s_audio.c` | I2S driver init + WAV playback via MAX98357A |
| `main/telegram/telegram_bot.c` | Long-poll Telegram bot |
| `main/bus/message_bus.c` | FreeRTOS inbound/outbound queues |
| `main/memory/psram_alloc.h` | PSRAM/SRAM allocation wrappers |
| `main/cli/serial_cli.c` | Full UART REPL with all config commands |
| `sdkconfig.defaults.esp32s3` | ESP32-S3 SDK defaults (flash, PSRAM, partitions) |
| `partitions_s3_32mb.csv` | Flash partition map |
| `littlefs_data/` | Static filesystem content (baked into littlefs.bin) |
