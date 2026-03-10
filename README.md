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
| USB webcam (UVC + UAC) | Vision input + microphone via USB-A port |

### Voice input options

| Method | How it works |
|--------|-------------|
| **Browser voice** | Click Record in the web UI — browser sends WebM audio, Groq Whisper transcribes |
| **Webcam PTT** | Hold the BOOT button (GPIO0) while speaking into the webcam's built-in mic — release to transcribe |

> The webcam PTT path works with any USB composite device that exposes a UAC (USB Audio Class) microphone alongside UVC video — most modern webcams qualify. The LED turns white while listening and blue when the agent is thinking.

**INMP441 mic + wake word ("Hi ESP")** — supported in firmware (WakeNet9 AFE) but not currently wired due to power constraints. Active only when no UAC webcam is detected at boot.

### Pin Map

| GPIO | Signal | Connected to |
|------|--------|-------------|
| 0 | PTT button | BOOT button (active low, built-in pull-up) |
| 15 | I2S BCLK | MAX98357A BCLK + INMP441 SCK *(not currently wired)* |
| 16 | I2S LRCLK | MAX98357A LRC + INMP441 WS *(not currently wired)* |
| 17 | I2S DOUT | MAX98357A DIN (speaker) *(not currently wired)* |
| 18 | I2S DIN | INMP441 SD (mic) *(not currently wired)* |
| 19 | USB D− | USB OTG host (webcam) |
| 20 | USB D+ | USB OTG host (webcam) |
| 38 | LED | WS2812 NeoPixel status indicator |
| 42 | AMP_SD | MAX98357A shutdown *(not currently wired)* |
| 43 | UART0 TX | Serial CLI |
| 44 | UART0 RX | Serial CLI |

### LED states

| Color | State |
|-------|-------|
| Red (solid) | Booting |
| Yellow (blink) | Connecting to WiFi |
| Green (breathing) | Ready / idle |
| Blue (pulse) | Agent thinking |
| Cyan (pulse) | Generating TTS |
| White (pulse) | Listening (PTT held) |
| White (flash → fade) | Camera capture flash |
| Red (fast flash) | Error |

### Wiring — MAX98357A (speaker amp) *(not currently wired)*

```
VIN  → 5 V (USB rail — keeps amp peak current off the 3.3 V rail)
GND  → GND
DIN  → GPIO 17
BCLK → GPIO 15
LRC  → GPIO 16
SD   → GPIO 42
GAIN → GND (3 dB; floating = 12 dB)
```

Speaker wires: red → + terminal, black → − terminal. **Do not** connect the speaker's − to system GND (bridge-tied output).

### Wiring — INMP441 (microphone) *(not currently wired)*

```
VDD → 3.3 V  (max 3.6 V — never 5 V)
GND → GND
SD  → GPIO 18
SCK → GPIO 15 (shared with MAX98357A)
WS  → GPIO 16 (shared with MAX98357A)
L/R → GND (left channel)
```

### Wiring — USB webcam

Wire a USB-A female connector:
- D− → GPIO 19, D+ → GPIO 20 (handled by the internal USB OTG transceiver)
- VBUS (5 V, up to 500 mA) from an external 5 V rail — **not** from an ESP32 GPIO

Most cheap UVC-compatible webcams work. For PTT mic, the webcam must also expose a UAC audio interface (most do). The ESP32's USB OTG is Full-Speed only (12 Mbps) — High-Speed-only devices will not enumerate.

---

## Features

| Feature | Details |
|---------|---------|
| **LLM** | Claude (Anthropic), OpenAI, or any OpenRouter model — token-by-token streaming to browser; 4096 max output tokens; 64 KB context buffer |
| **Wake word** | "Hi ESP" via WakeNet9 + AFE — supported in firmware, requires INMP441 (not currently wired) |
| **STT** | Groq Whisper — browser WebM audio or webcam UAC PCM |
| **TTS** | Groq PlayAI — WAV cached in PSRAM, served to browser at `/tts/<id>` (local speaker not currently wired) |
| **Vision** | USB webcam → MJPEG frame → Claude vision API → spoken description |
| **Webcam PTT** | Hold BOOT button → speak into webcam mic → release → agent responds |
| **Telegram** | Long-poll bot — full conversation with the same agent |
| **WebSocket UI** | Browser voice interface at `https://langoustine.local` (WSS) |
| **Home Assistant** | Query entity state and call services (lights, switches, climate, etc.) |
| **Klipper / Moonraker** | 3D printer status, temps, print progress, gcode control |
| **Push notifications** | ntfy.sh push to phone — agent-initiated or rule-triggered |
| **Cron** | Agent-scheduled recurring and one-shot jobs |
| **Rules engine** | Condition/action automations (temp alerts, HA triggers, …) |
| **OTA** | Firmware update over WiFi via CLI or HTTP |
| **mDNS** | Reachable as `langoustine.local` |
| **Serial CLI** | Full REPL on UART0 (115200 baud) |
| **Monitor panel** | Real-time event stream (LLM provider/model, tool calls, search provider) |

### Built-in agent tools

`web_search` · `get_current_time` · `read_file` · `write_file` · `edit_file` · `list_dir` · `search_files` · `http_request` · `send_email` · `cron_add/list/remove` · `ha_request` · `klipper_request` · `gpio_read/write/mode` · `wifi_scan` · `rss_fetch` · `device_temp` · `system_info` · `memory_write` · `memory_read` · `memory_append_today` · `rule_create/list/delete` · `capture_image` · `send_notification` · `get_weather` · `device_restart` · `session_clear`

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

# 3. Download components, generate sdkconfig, and build
idf.py update-dependencies
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
  0x830000 build/srmodels/srmodels.bin \
  0xa30000 build/littlefs.bin
```

### Flash (app only — subsequent updates)

```bash
# OTA (no cable needed):
SKIP_BUILD=1 bash scripts/ota_deploy.sh <device-ip>

# Or via serial:
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-* -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m \
  0x20000 build/langoustine.bin
```

---

## Configuration

### Core secrets (no rebuild needed)

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

### Third-party service credentials (`/lfs/config/SERVICES.md`)

Edit this file via the web UI or serial CLI (`write_file`) to configure integrations:

```markdown
## Home Assistant
ha_url: http://192.168.0.29:8123
ha_token: <long-lived-access-token>

## Klipper / Moonraker
moonraker_url: http://192.168.0.50:7125
moonraker_apikey:

## Email
smtp_host: smtp.gmail.com
smpt_port: 587
username: you@gmail.com
password: <app-password>
from_address: you@gmail.com
to_address: you@me.com
```

### LLM providers

```
lango> set_api_key <key>        # sets provider to "anthropic" (default)
```

To switch providers, `POST /api/config` with `{"provider":"openai"}` or `{"provider":"openrouter"}`. OpenRouter gives access to hundreds of models with a single key.

---

## Usage

### Browser voice UI

Navigate to `https://langoustine.local` (or `https://<device-ip>`).

> **First visit:** the device uses a self-signed TLS certificate. In Safari: *Show Details → Visit Website → confirm in Keychain*. In Chrome: type `thisisunsafe` on the warning page.

- **Voice mode:** Click **Record**, speak, click **Stop** — the browser sends WebM audio, Groq Whisper transcribes it, the LLM replies, and TTS audio plays back in the browser.
- **Text mode:** Type a message and press Enter. Tokens stream word-by-word as the LLM generates.
- The developer console at `/console` shows a real-time monitor feed: LLM provider/model, tool calls, search provider, heartbeat events.

### Webcam PTT (push-to-talk)

With a UAC+UVC webcam plugged into the USB-A port:

1. The serial log shows `UAC device connected` on plug-in.
2. Hold the **BOOT button** (GPIO0) — LED turns white.
3. Speak into the webcam's microphone.
4. Release the button — LED turns blue (thinking), then green when done.
5. The response plays as TTS in the browser and/or Telegram.

Maximum recording: 8 seconds (auto-commits). Browser voice continues to work independently.

### Telegram

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy the token.
2. `lango> set_tg_token <token>` — restart to activate.
3. Start a conversation — the bot uses the same agent and full tool set.

### Home Assistant

Configure `ha_url` and `ha_token` in SERVICES.md, then ask naturally:

```
"Turn off the kitchen lights"
"What's the temperature in the living room?"
"Is the front door locked?"
"Set the thermostat to 72°F"
```

The `ha_request` tool makes REST calls to your local HA instance directly over LAN. Dangerous endpoints (restart, delete) are blocked.

### Klipper / 3D printer

Configure `moonraker_url` in SERVICES.md (must include port `:7125`):

```
"What's the printer doing?"
"What's the bed temperature?"
"How much filament is left on this print?"
"Pause the print"
```

The `klipper_request` tool calls Moonraker's REST API. Machine-level endpoints (system reboot, file delete) are blocked.

### Push notifications (ntfy.sh)

Configure a topic, then ask the agent to alert you:

```
lango> set_notify_topic my-langoustine-alerts
lango> set_notify_server https://ntfy.sh   # optional — default is ntfy.sh
```

Install the [ntfy app](https://ntfy.sh) on your phone and subscribe to the same topic. Then:

```
"Notify me when the print finishes"
"Send me a push notification if the temperature drops below 60°F"
"Alert me via push when the front door opens"
```

The `send_notification` tool supports title, priority (`urgent`/`high`/`default`/`low`/`min`), and emoji tags. You can also self-host ntfy.

### Webcam

With a UVC webcam connected to the USB-A port:

```
lango> capture                          # saves latest.jpg to LittleFS
GET https://langoustine.local/camera/latest.jpg   # view in browser
```

Ask the agent "What do you see?" and it will capture a frame, send it to the Claude vision API, and speak the description. The LED flashes white on capture then fades.

### Serial CLI

Connect at 115200 baud. Type `help` for a full command list. Key commands:

```
wifi_status              show connection and IP
config_show              all settings with their source (NVS / build)
capture                  grab a JPEG from the USB webcam → /lfs/captures/latest.jpg
tool_exec <n> '{…}'      run any registered tool directly
memory_read              dump MEMORY.md
ota <url>                update firmware over WiFi
set_notify_topic <t>     set default ntfy push topic
set_notify_server <url>  set ntfy server (for self-hosted)
restart                  reboot
```

---

## Filesystem layout (`/lfs`)

```
/lfs/config/    SOUL.md      — system prompt / personality
                USER.md      — user name, timezone, preferences
                SERVICES.md  — third-party credentials (HA, Klipper, email…)
/lfs/memory/    MEMORY.md    — long-term episodic memory (16 KB max)
/lfs/sessions/  <chat_id>.json — per-user conversation history
/lfs/skills/    *.md         — skill definitions loaded by the agent
/lfs/captures/  latest.jpg   — most recent webcam capture
/lfs/console/   index.html, dev.html
/lfs/cron.json               — scheduled jobs
/lfs/HEARTBEAT.md            — periodic task checklist (every 30 min)
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
[Browser WSS]──────▶ ws_server.c ──▶ message_bus (inbound)
[Wake word / PTT]──▶ wake_word.c ──▶ STT task ──▶ message_bus (inbound)
[Webcam PTT]───────▶ uac_ptt_task ─▶ STT task ──▶ message_bus (inbound)
[Telegram]─────────▶ telegram_bot.c ─▶ message_bus (inbound)
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
| 0 | WiFi/lwIP, HTTPS+WSS server, outbound dispatch, Telegram polling, serial CLI, UAC PTT task |
| 1 | Agent loop (LLM), STT task, TTS task, cron service |

### Audio pipeline — browser voice

1. Browser records via Web Speech API (WebM/Opus)
2. Binary WebSocket frames → `audio_ring_append()` (256 KB PSRAM ring buffer)
3. `audio_end` → STT task POSTs to Groq Whisper → transcript pushed as inbound message
4. Agent replies → `tts_generate()` POSTs to Groq PlayAI → WAV cached in PSRAM (4 slots, 512 KB each, 5-min TTL)
5. `{"type":"message","tts_id":"<8hex>"}` sent to browser → browser fetches `GET /tts/<id>` and plays WAV

### Audio pipeline — webcam PTT

1. USB host enumerates webcam composite device (UVC + UAC interfaces)
2. UAC driver opens the RX interface; stream is started on BOOT button press
3. `uac_host_device_read()` in PTT task → `audio_ring_append()` (same PSRAM ring)
4. Button release → `audio_ring_patch_wav_sizes()` + `audio_ring_commit("ptt")` → STT task wakes
5. Same STT → agent → TTS path as browser voice

### Flash partition map

| Partition | Offset | Size |
|-----------|--------|------|
| nvs | 0x009000 | 64 KB |
| otadata | 0x01A000 | 8 KB |
| ota\_0 | 0x020000 | 4 MB |
| ota\_1 | 0x420000 | 4 MB |
| coredump | 0x820000 | 64 KB |
| model | 0x830000 | 2 MB (WakeNet9 "Hi ESP") |
| littlefs | 0xA30000 | ~22 MB |

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

## Changelog

### 2026-03-10 — Stability: log_buffer PSRAM crash + OTA stack
- **`main/diag/log_buffer.c`** — Ring buffer (`s_ring`) moved from PSRAM to SRAM (`heap_caps_malloc MALLOC_CAP_INTERNAL`). PSRAM is inaccessible while the MMU cache is disabled during OTA flash writes; the vprintf hook firing in that window caused a Data Load/Store Error panic. 8 KB from SRAM resolves this entirely.
- **`main/diag/log_buffer.c`** — `volatile int s_busy` → `_Atomic int`; `log_buffer_get()` now snapshots `s_head`/`s_fill` under the atomic busy flag to prevent torn reads.
- **`main/ota/ota_manager.c`** — OTA task stack increased 10 KB → 14 KB. mbedTLS TLS handshake peaks at ~10–12 KB; the previous limit was at the edge of a stack overflow.

---

## License

MIT — see [LICENSE](LICENSE).
