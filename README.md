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
| SSD1306 OLED (128×64) | Ambient status dashboard — IP, uptime, heap, agent state |
| MAX98357A + speaker | I2S audio output *(optional — enable with `LANG_I2S_AUDIO_ENABLED`)* |
| INMP441 microphone | I2S audio input + wake word *(optional — fallback when no UAC mic)* |

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
| 3 | I2S BCLK | MAX98357A BCLK + INMP441 SCK (shared bus) |
| 4 | I2S LRCLK | MAX98357A LRC + INMP441 WS (shared bus) |
| 5 | I2S DIN (RX) | INMP441 SD (microphone) |
| 6 | I2S DOUT (TX) | MAX98357A DIN (speaker) |
| 9 | I2C SDA | SSD1306 OLED + future peripherals |
| 10 | I2C SCL | SSD1306 OLED + future peripherals |
| 19 | USB D− | USB OTG host (webcam) |
| 20 | USB D+ | USB OTG host (webcam) |
| 38 | LED | WS2812 NeoPixel status indicator |
| 42 | AMP_SD | MAX98357A shutdown/enable |
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

### Wiring — SSD1306 OLED (128×64, I2C)

```
VCC → 3.3 V
GND → GND
SDA → GPIO 9
SCL → GPIO 10
```

Address auto-detected at boot (0x3C or 0x3D). I2C bus runs at 100 kHz (safe for dupont wires). The display shows IP address, uptime, SRAM/PSRAM free, agent state, and tool calls in real time.

### Wiring — MAX98357A (speaker amp)

```
VIN  → 5 V (USB rail — keeps amp peak current off the 3.3 V rail)
GND  → GND
DIN  → GPIO 6
BCLK → GPIO 3
LRC  → GPIO 4
SD   → GPIO 42
GAIN → GND (3 dB; floating = 12 dB)
```

Speaker wires: red → + terminal, black → − terminal. **Do not** connect the speaker's − to system GND (bridge-tied output).

### Wiring — INMP441 (microphone)

```
VDD → 3.3 V  (max 3.6 V — never 5 V)
GND → GND
SD  → GPIO 5
SCK → GPIO 3 (shared with MAX98357A)
WS  → GPIO 4 (shared with MAX98357A)
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
| **LLM** | Claude (Anthropic), OpenAI, OpenRouter, or local Ollama — token-by-token streaming to browser; 4096 max output tokens; 64 KB context buffer |
| **Wake word** | "Hi ESP" via WakeNet9 + AFE — requires INMP441 mic |
| **STT** | Local-first via mlx-audio (or any OpenAI-compatible server), falls back to Groq Whisper; browser WebM audio or webcam UAC PCM; local mic audio Opus-compressed before upload |
| **TTS** | Local-first via mlx-audio (Kokoro, etc.), falls back to Groq PlayAI; WAV cached in PSRAM, served to browser at `/tts/<id>`; optional local I2S speaker playback |
| **OLED display** | SSD1306 128×64 I2C — ambient dashboard showing IP, uptime, heap, agent state, tool calls |
| **Vision** | USB webcam → MJPEG frame → Claude vision API → spoken description |
| **Webcam PTT** | Hold BOOT button → speak into webcam mic → release → agent responds |
| **Telegram** | Long-poll bot — full conversation with the same agent |
| **WebSocket UI** | Browser voice interface at `http://langoustine.local` with quick-action dashboard |
| **Home Assistant** | Query entity state and call services (lights, switches, climate, etc.) |
| **Klipper / Moonraker** | 3D printer status, temps, print progress, gcode control |
| **Push notifications** | ntfy.sh push to phone — agent-initiated or rule-triggered |
| **Cron** | Agent-scheduled recurring and one-shot jobs |
| **Rules engine** | Condition/action automations (temp alerts, HA triggers, …) |
| **OTA** | Firmware update over WiFi via CLI or HTTP |
| **mDNS** | Reachable as `langoustine.local` |
| **Serial CLI** | Full REPL on UART0 (115200 baud) |
| **Rate limiting** | Configurable LLM API rate limit (default 60/hour); `rate_limit` CLI command |
| **WiFi onboarding** | Captive portal on first boot (no credentials) — SoftAP with setup page |
| **MCP server** | JSON-RPC 2.0 MCP protocol at `/mcp` — exposes all 33 tools to Claude Desktop, Cursor, and other MCP clients |
| **Quick actions** | Dashboard cards for common tasks (briefing, surf check, capture, system info) — one-tap from the web UI |
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

### Local model (Ollama)

Run a local LLM on your network for free, zero-latency inference. Add to `/lfs/config/SERVICES.md`:

```markdown
## Local Model
base_url: http://192.168.0.25:11434/v1
api_key: ollama
model: qwen2.5:14b
```

Then switch to it:

```
lango> set_model_provider ollama
lango> set_model qwen2.5:14b
```

Or set the URL directly: `lango> set_local_url http://192.168.0.25:11434/v1`

The Ollama provider uses the OpenAI-compatible `/v1/chat/completions` endpoint over plain HTTP (no TLS overhead). Any model served by Ollama works — tool calling support depends on the model.

### Local audio (mlx-audio / Piper / whisper.cpp)

Run TTS and STT on your local network for near-zero latency and no per-request cost. Add to `/lfs/config/SERVICES.md`:

```markdown
## Local Audio (mlx-audio)
base_url: http://192.168.0.25:8000
```

Any server exposing OpenAI-compatible `/v1/audio/speech` (TTS) and `/v1/audio/transcriptions` (STT) endpoints works:

| Server | TTS | STT | Notes |
|--------|-----|-----|-------|
| [mlx-audio](https://github.com/Blaizzy/mlx-audio) | ✅ Kokoro, Qwen3-TTS | ✅ Whisper, Parakeet | Apple Silicon optimized; see setup below |
| [Piper](https://github.com/rhasspy/piper) | ✅ | ❌ | Blazing fast CPU TTS (~200ms); needs a wrapper for OpenAI API |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | ❌ | ✅ | `whisper-server --port 8080` |

**mlx-audio setup (Apple Silicon Mac):**

```bash
pip install mlx-audio

# Start the OpenAI-compatible server (models download automatically on first use)
/Library/Frameworks/Python.framework/Versions/3.12/bin/python3 \
    -m mlx_audio.server --host 0.0.0.0 --port 8000
```

Then set your Mac's LAN IP in SERVICES.md (find it with `ipconfig getifaddr en0`):

```markdown
## Local Audio (mlx-audio)
base_url: http://192.168.x.x:8000
```

The device tries local first over plain HTTP (no TLS overhead), then falls back to Groq cloud if the local server is unreachable. After a failure, local is skipped for 60 seconds before retrying.

---

## Usage

### Browser voice UI

Navigate to `http://lango.local` (or `http://<device-ip>`).

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
| Ollama *(optional)* | Free local LLM inference | [ollama.com](https://ollama.com) |
| mlx-audio *(optional)* | Free local TTS + STT (Apple Silicon) | [github.com/Blaizzy/mlx-audio](https://github.com/Blaizzy/mlx-audio) |

Groq offers a generous free tier that covers typical personal-assistant usage.
The `set_search_key` CLI command accepts either a Tavily key (`tvly-…`) or a Brave key — the provider is detected automatically from the key prefix.

---

## Changelog

### 2026-03-26 — MCP server, quick actions dashboard

- **MCP server** (`main/mcp/mcp_server.c`) — JSON-RPC 2.0 endpoint at `POST /mcp` implementing the Model Context Protocol (2024-11-05). Supports `initialize`, `tools/list`, and `tools/call` methods. All 33 registered agent tools are automatically exposed with their JSON schemas — no per-tool MCP glue needed. Any MCP client (Claude Desktop, Cursor, etc.) can connect and invoke tools directly on the device. Tool output buffer: 16 KB from PSRAM.
- **Quick actions dashboard** (`littlefs_data/console/index.html`) — Tap-to-run action cards on the main web UI: Morning Briefing, Surf Check, Capture Image, System Info, plus a custom prompt field. Each card sends a pre-built message to the agent via WebSocket. Cards are styled with emoji headers and a responsive grid layout.
- **Dev console improvements** (`littlefs_data/console/dev.html`) — Consolidated monitor panel with auto-reconnect, connection status indicator, and improved event formatting.

### 2026-03-26 — Brave Search, search caching, smart LLM routing, SRAM guard tuning

- **Brave Search provider** (`main/tools/tool_web_search.c`) — Web search now supports both Tavily and Brave Search, routed by API key prefix: keys starting with `tvly-` use Tavily, all others use Brave. Set via `set_search_key <key>` CLI command or SERVICES.md. Brave Search uses the `/res/v1/web/search` API with `X-Subscription-Token` auth.
- **Search result caching** — 8-slot LRU cache in PSRAM with 5-minute TTL, keyed by FNV-1a query hash. Identical queries within the TTL window return cached results instantly (zero cost, zero latency). Cache hits shown as "cached" in the monitor panel. Particularly beneficial for morning briefings where retry/similar queries are common.

### 2026-03-26 — Smart LLM routing, SRAM guard tuning, auto-email, stream timeout

- **Smart LLM routing** (`main/agent/agent_loop.c`) — Channel-aware and complexity-aware model selection. `system` channel (heartbeat, cron) always routes to cloud to avoid Ollama hangs on multi-tool chains. Text channel now detects complex requests (briefing, email, search, research, forecast, headlines) and routes those to cloud too. Voice channel (`ptt`) continues to use configurable `voice_provider`/`voice_model`.
- **Vision-aware local model selection** — When a turn calls `capture_image`, subsequent LLM routing uses `llm_get_local_model()` (default `qwen3-vl:8b`). Plain-text turns use the new `local_text_model` (default `gemma3:12b`). Configure both in SERVICES.md `## Local Model` section: `model:` (vision) and `local_text_model:` (text-only).
- **LLM stream hard timeout** (`main/llm/llm_proxy.c`) — Per-stream deadline enforced in the SSE event handler: 3 minutes for local (Ollama), 90 seconds for cloud. Prevents indefinite hangs when a slow model streams tokens arbitrarily slowly. Previous behaviour: a 7-search briefing on qwen3-vl:8b could block for 800+ seconds.
- **Auto-email long responses** — WebSocket channel responses longer than 200 characters are automatically emailed to the configured `to_address` (via `tool_smtp`). Subject line is derived from the first 60 characters of the response. Zero additional LLM turns consumed.
- **SRAM guard threshold lowered** — Hard-restart threshold reduced from 22 KB to 14 KB; warning-only threshold reduced from 28 KB to 24 KB. Empirically, turns complete successfully at a heap_min of 13.9 KB. The previous 22 KB threshold was falsely triggering restarts (~18.6 KB free) after heavy briefing turns due to normal mbedTLS session-ticket cache accumulation — not an actual leak.
- **Per-channel LLM timeout** — System channel turns allow 300 s (multi-tool chains); interactive turns allow 180 s.
- **`local_text_model` config key** — New SERVICES.md key parsed in both boot and hot-reload paths. Allows separate Ollama models for vision turns (`model:`) and text-only turns (`local_text_model:`).

### 2026-03-25 — Drop TLS, plain HTTP/WS on port 80

- **`main/gateway/ws_server.c`** — replaced `esp_https_server` + `httpd_ssl_start()` with plain `httpd_start()` on port 80. WebSocket clients connect via `ws:` instead of `wss:`. Removes ~20–30 KB SRAM overhead per TLS session; idle SRAM rises from ~27 KB to ~47 KB. Browser UI automatically switches to `ws:` (protocol-relative JS detection). `max_uri_handlers` raised from 25 → 27 (26 routes registered).
- **`main/langoustine.c`** — mDNS now advertises `_http._tcp` on port 80 (was `_https._tcp:443`). Navigate to `http://lango.local`.
- **`main/CMakeLists.txt`** — removed `EMBED_TXTFILES` for TLS cert/key PEMs; removed `esp_https_server` from REQUIRES.
- **`main/display/oled_display.c`** — OLED IP display now abbreviates to last 2 octets (e.g. `192.168.0.44` → `0.44`) to fit the 128px display alongside the large clock.

---

### 2026-03-25 — Fix: stt_task stack overflow crash with local STT/Opus

- **`main/audio/audio_pipeline.c`** — `stt_task` stack increased 16 KB → 28 KB SRAM. Coredump analysis showed the task using 18.5 KB (overflow by ~2 KB), corrupting the adjacent wifi task TCB and causing a `LoadProhibitedCause` fault in the FreeRTOS scheduler (`a2 = 0xa5a5a5a5` fill pattern). The overflow is triggered when local STT is configured: `stt_transcribe_local()` creates a fresh `esp_http_client` on top of the `opus_encode_pcm_to_ogg()` stack frame, exceeding the previous 16 KB limit.

---

### 2026-03-23 — Local-first audio, OLED dashboard, Ollama, Opus, rate limiting, WiFi onboarding
- **Local-first TTS/STT** — New `## Local Audio` section in SERVICES.md. Device tries local server (mlx-audio, Piper, whisper.cpp — any OpenAI-compatible `/v1/audio/speech` and `/v1/audio/transcriptions`) over plain HTTP first, falls back to Groq cloud on failure. 60s backoff after local failure. Eliminates TLS overhead and cloud latency for voice pipeline.
- **OLED dashboard overhaul** — IP address at top of display with RSSI signal bar below; date on its own row (no overlap). Active screen shows provider/model, status, message preview, and token counts. Idle screen shows time, date, provider, stats, uptime.
- **SSD1306 OLED display** — 128×64 I2C dashboard showing IP, uptime, heap stats, agent state, and active tool calls. Auto-probes 0x3C/0x3D at boot; I2C bus scan logged. White test pattern on first init. Auto-clears stale messages after 30s.
- **Local LLM via Ollama** — New `ollama` provider for free, local inference. Configure base URL in SERVICES.md `## Local Model` section. Uses OpenAI-compatible `/v1/chat/completions` over plain HTTP (no TLS). Smart routing: tries local Ollama first, falls back to cloud if offline. CLI: `set_model_provider ollama`, `set_local_url <url>`.
- **TTS text limit raised** — `TTS_MAX_CHARS` increased from 500 to 1500 to prevent truncation of longer spoken responses.
- **Opus encoding for local mic** — WAV audio from INMP441/UAC mic compressed to Opus/OGG before STT upload (~10–15× size reduction). Browser WebM/Opus audio passed through as-is.
- **LLM rate limiting** — 60 requests/hour default (configurable via `rate_limit` CLI command). System channel messages (cron, heartbeat) exempt. Returns user-facing message when limit reached.
- **WiFi onboarding captive portal** — On first boot with no stored credentials, starts a SoftAP "Langoustine-XXXX" with a web form for WiFi SSID/password, API key, and Telegram token. Saves to NVS and reboots.
- **I2C bus scan** — Full 7-bit address scan at boot with diagnostic logging; results written to `/lfs/i2c_diag.txt`.
- **I2C clock reduced** — 400 kHz → 100 kHz (safer with dupont jumper wires).

### 2026-03-16 — Hardening: naming cleanup, auth guards, shared SERVICES.md parser
- **`main/bus/message_bus.h`** — Renamed `mimi_msg_t` → `lang_msg_t` and `MIMI_CHAN_*` → `LANG_CHAN_*` across the entire codebase (compat aliases retained in `langoustine_config.h`).
- **`main/config/services_parser.c/.h`** — New shared `services_parse_section()` utility extracted from duplicated SERVICES.md parsing logic in tool\_ha, tool\_klipper, and tool\_smtp.
- **`main/gateway/ws_server.c`** — Added `request_is_authed()` guards to 8 previously unprotected endpoints: `/api/file`, `/api/heartbeat`, `/api/sysinfo`, `/api/config` (GET), `/api/crons`, `/api/logs`, `/api/say`, `/api/speaker-test`. Expanded CORS headers (Methods, Headers, Max-Age).
- **`main/proxy/http_proxy.c`**, **`main/telegram/telegram_bot.c`**, **`main/tools/tool_web_search.c`** — Replaced `ESP_ERROR_CHECK()` in NVS write paths with graceful error handling and logging (prevents panic on NVS corruption).
- **`main/tools/tool_web_search.c`** — Eliminated intermediate PSRAM allocation in `search_tavily()`: results now write directly into the caller's output buffer. Added null-check on `esp_http_client_init()`.

### 2026-03-10 — Stability: log_buffer PSRAM crash + OTA stack
- **`main/diag/log_buffer.c`** — Ring buffer (`s_ring`) moved from PSRAM to SRAM (`heap_caps_malloc MALLOC_CAP_INTERNAL`). PSRAM is inaccessible while the MMU cache is disabled during OTA flash writes; the vprintf hook firing in that window caused a Data Load/Store Error panic. 8 KB from SRAM resolves this entirely.
- **`main/diag/log_buffer.c`** — `volatile int s_busy` → `_Atomic int`; `log_buffer_get()` now snapshots `s_head`/`s_fill` under the atomic busy flag to prevent torn reads.
- **`main/ota/ota_manager.c`** — OTA task stack increased 10 KB → 14 KB. mbedTLS TLS handshake peaks at ~10–12 KB; the previous limit was at the edge of a stack overflow.

---

## License

MIT — see [LICENSE](LICENSE).
