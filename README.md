# Langoustine

An ESP32-S3 AI assistant with voice, Telegram, and WebSocket interfaces — running entirely on a $10 microcontroller.

Talk to it through a browser, a Telegram bot, or a serial terminal. It hears you through a MEMS microphone, thinks with Claude (or any OpenAI-compatible LLM), speaks through an I2S amplifier, and sees through a USB webcam. Everything runs on-device with no cloud middleware.

---

## Hardware

**Board:** ESP32-S3-WROOM-2-N32R16V
- 240 MHz dual-core Xtensa LX7
- 32 MB Octal Flash
- 16 MB Octal PSRAM
- Built-in USB OTG (Full Speed, 12 Mbps)

**Peripherals:**

| Module | Purpose |
|--------|---------|
| MAX98357A | I2S Class-D amplifier → speaker |
| INMP441 | I2S MEMS microphone |
| USB webcam (UVC) | Vision input via USB-A port |

### Pin Map

| GPIO | Signal | Connected to |
|------|--------|-------------|
| 15 | I2S\_BCLK | MAX98357A BCLK + INMP441 SCK |
| 16 | I2S\_LRCLK | MAX98357A LRC + INMP441 WS |
| 17 | I2S\_DOUT | MAX98357A DIN (speaker) |
| 18 | I2S\_DIN | INMP441 SD (mic) |
| 19 | USB D− | USB OTG host (webcam) |
| 20 | USB D+ | USB OTG host (webcam) |
| 43 | UART0 TX | Serial CLI |
| 44 | UART0 RX | Serial CLI |

### Wiring — MAX98357A

```
MAX98357A   →   ESP32-S3
VIN         →   3.3V
GND         →   GND
DIN         →   GPIO 17
BCLK        →   GPIO 15
LRC         →   GPIO 16
SD          →   leave floating (amp enabled by default)
```
Connect speaker + wire to the + terminal and − wire to the − terminal.
**Do not** tie the − terminal to system GND (bridge-tied output).

### Wiring — INMP441

```
INMP441   →   ESP32-S3
VDD       →   3.3V  (max 3.6 V — never 5 V)
GND       →   GND
SD        →   GPIO 18
SCK       →   GPIO 15
WS        →   GPIO 16
L/R       →   GND  (left channel)
```

### Wiring — USB webcam

Wire a USB-A female connector:
- D− → GPIO 19, D+ → GPIO 20 (handled by the internal USB OTG transceiver)
- VBUS (5 V, up to 500 mA) from an external 5 V rail — **not** from an ESP32 GPIO

Most cheap UVC-compatible webcams work (Logitech C270, etc.).

---

## Features

| Feature | Details |
|---------|---------|
| **LLM** | Claude (Anthropic), OpenAI, or any OpenRouter model — streaming responses |
| **STT** | Groq Whisper — browser streams WebM audio, transcribed on Groq |
| **TTS** | Groq PlayAI — spoken reply cached in PSRAM, played via MAX98357A |
| **Vision** | USB webcam → MJPEG frame → Claude vision API → spoken description |
| **Telegram** | Long-poll bot — full conversation with the same agent |
| **WebSocket UI** | Browser voice interface at `http://langoustine.local` |
| **Cron** | Agent-scheduled recurring and one-shot jobs |
| **Rules engine** | Condition/action automations (temp alerts, HA triggers, …) |
| **OTA** | Firmware update over WiFi via CLI or HTTP |
| **mDNS** | Reachable as `langoustine.local` |
| **Serial CLI** | Full REPL on UART0 (115200 baud) |

### Built-in agent tools (25+)

`web_search` · `get_current_time` · `read_file` · `write_file` · `edit_file` · `list_dir` · `search_files` · `http_request` · `send_email` · `cron_add/list/remove` · `ha_request` · `klipper_request` · `gpio_read/write/mode` · `wifi_scan` · `rss_fetch` · `device_temp` · `system_info` · `memory_write` · `memory_append_today` · `rule_create/list/delete` · **`capture_image`**

---

## Prerequisites

- **ESP-IDF 6.0+** — [install guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
- Python 3.8+ (bundled with ESP-IDF)
- API keys for Anthropic and Groq (minimum — see [API keys needed](#api-keys-needed))

---

## Build

```bash
# 1. Source ESP-IDF
source ~/esp/esp-idf/export.sh

# 2. Copy and fill in build-time secrets (optional — you can use the CLI instead)
cp main/langoustine_secrets.h.example main/langoustine_secrets.h
# edit main/langoustine_secrets.h

# 3. Generate sdkconfig (first time only)
idf.py set-target esp32s3

# 4. Build
idf.py build
```

This produces:
- `build/langoustine.bin` — application firmware
- `build/littlefs.bin` — LittleFS filesystem image (built from `littlefs_data/`)

### Flash (first time — full 32 MB layout)

```bash
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 32MB --flash_freq 80m \
  0x0      build/bootloader/bootloader.bin \
  0x8000   build/partition_table/partition-table.bin \
  0x1a000  build/ota_data_initial.bin \
  0x20000  build/langoustine.bin \
  0x830000 build/littlefs.bin
```

### Flash (subsequent updates)

```bash
idf.py -p /dev/tty.usbserial-* flash monitor
```

---

## Configuration

All secrets and settings can be applied **without a rebuild** via the serial CLI.

```
lango> set_wifi <ssid> <password>
lango> set_api_key <anthropic-or-openai-key>
lango> set_tg_token <telegram-bot-token>
lango> stt_key <groq-key>
lango> tts_key <groq-key>
lango> set_search_key <brave-api-key>   # optional — enables web_search tool
lango> config_show                       # verify everything
```

### Priority order (highest → lowest)

1. **NVS** — set at runtime via CLI or `POST /api/config`. Survives reboots.
2. **Build-time** — `main/langoustine_secrets.h` (gitignored; copy from `.example`).
3. **Defaults** — empty strings in `langoustine_config.h`.

### LLM providers

```
lango> set_api_key <key>        # sets provider to "anthropic" (default)
```

To switch providers, `POST /api/config` with `{"provider":"openai"}` or `{"provider":"openrouter"}`. OpenRouter gives access to hundreds of models with a single key.

### Supported models

Any model accessible via the configured provider. Defaults to `claude-opus-4-5`.

---

## Usage

### Browser voice UI

Navigate to `http://langoustine.local` (or the device IP on port 80).

- Click **Record** and speak — audio is streamed to the device, transcribed by Whisper, processed by the LLM, and the reply is spoken through the speaker and shown in the chat.
- The developer console at `/console` shows real-time tool calls and event logs.

### Telegram

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. `lango> set_tg_token <token>` — restart to activate.
3. Start a conversation — the bot uses the same agent and full tool set.

### Serial CLI

Connect at 115200 baud. Type `help` for a full command list. Key commands:

```
wifi_status          show connection and IP
config_show          all settings with their source (NVS / build)
capture              grab a JPEG from the USB webcam → /lfs/captures/latest.jpg
tool_exec <n> '{…}'  run any registered tool directly
memory_read          dump MEMORY.md
ota <url>            update firmware over WiFi
restart              reboot
```

### Webcam

With a UVC webcam connected to the USB-A port:

```
lango> capture                          # saves latest.jpg to LittleFS
GET http://langoustine.local/camera/latest.jpg   # view in browser
```

Ask the agent "What do you see?" and it will capture a frame, send it to the Claude vision API, and speak the description through the speaker.

---

## Filesystem layout (`/lfs`)

```
/lfs/config/    SOUL.md    — system prompt / personality
                USER.md    — user name, timezone, preferences
                SERVICES.md — third-party credentials (HA, email, Klipper…)
/lfs/memory/    MEMORY.md  — long-term episodic memory (6 KB max)
/lfs/sessions/  <chat_id>.json — per-user conversation history
/lfs/skills/    *.md       — skill definitions loaded by the agent
/lfs/captures/  latest.jpg — most recent webcam capture
/lfs/console/   index.html, dev.html
/lfs/cron.json             — scheduled jobs
/lfs/HEARTBEAT.md          — periodic task checklist (every 30 min)
```

Edit `SOUL.md` to change personality. Edit `USER.md` to set your name and timezone.

---

## Adding a tool

1. Create `main/tools/tool_<name>.c` and `.h` following the pattern of any existing tool (e.g. `tool_get_time.c`).
2. Implement `tool_<name>_execute(input_json, output, output_size)`.
3. Add `"tools/tool_<name>.c"` to `main/CMakeLists.txt`.
4. Include the header and register with `tool_registry_register()` in `main/tools/tool_registry.c`.

---

## Architecture

```
[Browser]──WebSocket──▶ ws_server.c ──▶ message_bus (inbound)
[Telegram]─────────────▶ telegram_bot.c ─▶ message_bus (inbound)
[Cron / Heartbeat]───────────────────────▶ message_bus (inbound)
                                                  │
                                          agent_loop.c  (Core 1)
                                          LLM + tool calls
                                                  │
                                         message_bus (outbound)
                                                  │
                               outbound_dispatch_task  (Core 0)
                              ┌────────────────────┴──────────────┐
                     ws_server_send()              telegram_send_message()
```

| Core | Tasks |
|------|-------|
| 0 | WiFi/lwIP, HTTP+WebSocket server, outbound dispatch, Telegram polling, serial CLI |
| 1 | Agent loop (LLM), STT task, TTS task, cron service |

### Flash partition map

| Partition | Offset | Size |
|-----------|--------|------|
| nvs | 0x009000 | 64 KB |
| otadata | 0x01A000 | 8 KB |
| ota\_0 | 0x020000 | 4 MB |
| ota\_1 | 0x420000 | 4 MB |
| coredump | 0x820000 | 64 KB |
| littlefs | 0x830000 | ~24 MB |

---

## API keys needed

| Service | Used for | Where to get one |
|---------|---------|-----------------|
| Anthropic | LLM + vision | [console.anthropic.com](https://console.anthropic.com) |
| Groq | STT (Whisper) + TTS (PlayAI) | [console.groq.com](https://console.groq.com) |
| Telegram | Bot interface | [@BotFather](https://t.me/BotFather) |
| Brave Search *(optional)* | Web search tool | [brave.com/search/api](https://brave.com/search/api/) |
| OpenRouter *(optional)* | Alternative LLM provider | [openrouter.ai](https://openrouter.ai) |

Groq offers a generous free tier that covers typical personal-assistant usage.

---

## License

MIT — see [LICENSE](LICENSE).
