# Langoustine

An ESP32-S3 AI assistant with voice, Telegram, and WebSocket interfaces — running entirely on a $10 microcontroller.

Talk to it through a browser, a Telegram bot, or a serial terminal. It thinks with Claude (or any OpenAI-compatible LLM), speaks back through your browser via Groq TTS, and sees through a USB webcam. Everything runs on-device with no cloud middleware.

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
| USB webcam (UVC) | Vision input via USB-A port |

> **Note:** Local I2S speaker/microphone hardware (MAX98357A + INMP441) has been removed.
> All audio is handled in the browser: the Web Speech API captures voice input, Groq Whisper
> transcribes it on-device, and TTS audio is streamed back to the browser as a WAV file.

### Pin Map

| GPIO | Signal | Connected to |
|------|--------|-------------|
| 19 | USB D− | USB OTG host (webcam) |
| 20 | USB D+ | USB OTG host (webcam) |
| 43 | UART0 TX | Serial CLI |
| 44 | UART0 RX | Serial CLI |

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
| **STT** | Groq Whisper — browser streams WebM audio, transcribed server-side |
| **TTS** | Groq PlayAI — spoken reply cached in PSRAM, served as WAV and played in browser |
| **Vision** | USB webcam → MJPEG frame → Claude vision API → spoken description |
| **Telegram** | Long-poll bot — full conversation with the same agent |
| **WebSocket UI** | Browser voice interface at `https://langoustine.local` (WSS) |
| **Cron** | Agent-scheduled recurring and one-shot jobs |
| **Rules engine** | Condition/action automations (temp alerts, HA triggers, …) |
| **OTA** | Firmware update over WiFi via CLI or HTTP |
| **mDNS** | Reachable as `langoustine.local` |
| **Serial CLI** | Full REPL on UART0 (115200 baud) |
| **Monitor panel** | Real-time event stream (LLM provider/model, tool calls, search provider) |

### Built-in agent tools (24+)

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

# 3. Generate sdkconfig and build
rm -f sdkconfig   # required when adding/removing components
idf.py set-target esp32s3
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

### Flash (app only — subsequent updates)

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-* -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m \
  0x20000 build/langoustine.bin
```

Or OTA (no cable needed):

```bash
SKIP_BUILD=1 bash scripts/ota_deploy.sh <device-ip>
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
lango> set_search_key <key>   # Tavily (tvly-…) or Brave — enables web_search tool
lango> config_show            # verify everything
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

Navigate to `https://langoustine.local` (or `https://<device-ip>`).

> **First visit:** the device uses a self-signed TLS certificate. In Safari: *Show Details → Visit Website → confirm in Keychain*. In Chrome: type `thisisunsafe` on the warning page.

- **Voice mode:** Click **Record**, speak, click **Stop** — the browser sends WebM audio, Groq Whisper transcribes it, the LLM replies, and TTS audio plays back in the browser.
- **Text mode:** Type a message and press Enter.
- The developer console at `/console` shows a real-time monitor feed: LLM provider/model, tool calls, search provider, heartbeat events.

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
GET https://langoustine.local/camera/latest.jpg   # view in browser
```

Ask the agent "What do you see?" and it will capture a frame, send it to the Claude vision API, and speak the description.

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
[Browser]──WSS──▶ ws_server.c ──▶ message_bus (inbound)
[Telegram]────────▶ telegram_bot.c ─▶ message_bus (inbound)
[Cron / Heartbeat]──────────────────▶ message_bus (inbound)
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
| 0 | WiFi/lwIP, HTTPS+WSS server, outbound dispatch, Telegram polling, serial CLI |
| 1 | Agent loop (LLM), STT task, TTS task, cron service |

### Audio pipeline (browser voice)

1. Browser records via Web Speech API (WebM/Opus)
2. Binary WebSocket frames → `audio_ring_append()` (256 KB PSRAM ring buffer)
3. `audio_end` → STT task POSTs to Groq Whisper → transcript pushed as inbound message
4. Agent replies → `tts_generate()` POSTs to Groq PlayAI → WAV cached in PSRAM (4 slots, 512 KB each, 5-min TTL)
5. `{"type":"message","tts_id":"<8hex>"}` sent to browser → browser fetches `GET /tts/<id>` and plays WAV

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
| Tavily *(optional)* | Web search tool | [tavily.com](https://tavily.com) |
| Brave Search *(optional)* | Web search tool (alternative) | [brave.com/search/api](https://brave.com/search/api/) |
| OpenRouter *(optional)* | Alternative LLM provider | [openrouter.ai](https://openrouter.ai) |

Groq offers a generous free tier that covers typical personal-assistant usage.
The `set_search_key` CLI command accepts either a Tavily key (`tvly-…`) or a Brave key — the provider is detected automatically from the key prefix.

---

## License

MIT — see [LICENSE](LICENSE).
