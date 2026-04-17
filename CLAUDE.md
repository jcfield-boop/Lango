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
4. STT task POSTs to local mlx-audio Whisper (if `stt_local_url` set) or Groq Whisper → transcript
5. Agent processes transcript → LLM (Apfel → Ollama → cloud, same hierarchy for voice and text) → response
6. TTS client tries local mlx-audio Kokoro first, falls back to Groq PlayAI → WAV cached in PSRAM
7. If `LANG_I2S_AUDIO_ENABLED`: WAV is played immediately via MAX98357A on GPIO 3/4/6
8. `{"type":"message","tts_id":"<8hex>"}` sent to browser → browser fetches `GET /tts/<id>`

**First-sentence TTS split** (on-device voice only, `chat_id=="ptt"`): before firing TTS, `agent_loop.c` scans the response for a sentence boundary (`. ! ? \n\n`) in `[40, 220]` chars with ≥20 chars of tail. If found, segment A is sent to Kokoro first and `i2s_audio_play_wav_async()` starts playback immediately; segment B's TTS is generated in parallel and appended via `i2s_audio_enqueue_wav()` — a 4-slot FreeRTOS queue in `i2s_audio.c` plays the two WAVs back-to-back. Cuts perceived voice latency ~400-600 ms on typical 2-3 sentence replies. Short replies fall back to the single-shot path.

**ReAct loop recovery** (`agent_loop.c`): if the LLM returns `stop=end_turn` with zero text bytes after one or more tool-use iterations, the agent injects a recovery turn ("Your response was cut off…") and retries once. This covers multi-step queries (daily briefing, web searches) where the model sometimes emits an empty final assistant turn. On voice turns, if the loop still has no final text, the error message is spoken aloud via TTS so the user hears feedback instead of silence.

**Local pipeline** (fully on-device Mac at `192.168.0.51`):
- STT: mlx-audio Whisper at `<base_url>/v1/audio/transcriptions` — raw WAV sent (Opus encoding bypassed)
- TTS: mlx-audio Kokoro at `<base_url>/v1/audio/speech` — model + voice configurable via SERVICES.md
- LLM (Ollama): at `<local_url>/v1/chat/completions` — full tool calling support. Uses `qwen3:8b` for text, `qwen3-vl:8b` for vision
- LLM (Apfel): Apple Foundation Model (~3B) via `apfel --serve --port 11435`. Ultra-fast (Neural Engine), no tools, minimal context (~400 tokens system prompt). Used for simple conversational queries on both voice and text channels.
- Cloud fallback: Groq (STT/TTS), OpenRouter (LLM) when local services unavailable

**Unified LLM routing** (in `agent_loop.c`) — same hierarchy for voice and text:
1. System/heartbeat/cron → cloud always (multi-step tool chains need speed). mlx-lm / Ollama / Apfel are **user-channel only**; scheduled traffic (heartbeat + cron system jobs) never hits the local tier. Pinned to `system_model` (default `openrouter`/`openai/gpt-4o-mini`) — `openrouter/auto` was occasionally routing to content-restricted models that refused `system_info` tasks.
2. Simple query (no tools needed) → **Apfel** if online (~1s response) → Ollama fallback → cloud fallback
3. Tool-triggering keywords (weather, remind, stock, etc.) → **Ollama** if online → cloud fallback
4. Complex keywords (briefing, email, research) → **cloud** directly
- Voice queries (`chat_id="ptt"`, wake word or PTT) add `max_tokens=400` + VOICE MODE prompt injection
- Cloud voice fallback uses `voice_provider`/`voice_model` (default: `openrouter`/`openai/gpt-4o-mini`)
- System channel uses `system_provider`/`system_model` (default: `openrouter`/`openai/gpt-4o-mini`) — configurable via `SERVICES.md` under `## Local Model`
- All LLM requests use `temperature=0.7` for focused, efficient generation

**Boot warmup tasks** (prevent cold-start latency):
- `stt_warmup_task`: 8s after boot, POSTs silent 20ms WAV to mlx-audio → pre-loads Whisper
- `tts_warmup_task`: 18s after boot, POSTs one-word request to mlx-audio Kokoro → pre-loads TTS model (90s timeout for cold load); eliminates ~60s first-request delay
- `llm_warmup_task`: 25s after boot, POSTs minimal chat to Ollama → pre-loads local model; primes 15s health cache

### I2S Bus Architecture

Simplex mode: TX on I2S_NUM_0 (master), RX on I2S_NUM_1 (slave). BCLK/WS shared physically on GPIO 3/4. Full-duplex `sig_loopback` mode caused permanent RX DMA stalls on ESP32-S3 — simplex avoids this entirely.

- **TX (I2S_NUM_0, master)**: drives BCLK/WS, feeds MAX98357A speaker via DOUT (GPIO 6). Silence pump task keeps BCLK alive when no audio is playing.
- **RX (I2S_NUM_1, slave)**: reads BCLK/WS as inputs, captures INMP441 mic via DIN (GPIO 5). `data_bit_width=32` matches INMP441's 24-bit-in-32-bit-slot format.
- **DMA workaround**: slave RX DMA only fills the ring buffer once after `i2s_channel_enable`. Each `i2s_audio_read()` call does disable+enable+30ms delay to restart DMA before reading.

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
lango> crashlog [clear]     # dump or truncate /lfs/memory/crashlog.md
```

All constants live in `main/langoustine_config.h` under the `LANG_` prefix.

### Crash Log

Abnormal resets (panic/int_wdt/task_wdt/wdt/brownout) are appended to `/lfs/memory/crashlog.md` by `log_crash_if_needed()` in `main/langoustine.c` on boot. Retrieval:
- `curl http://192.168.0.44/api/crashlog` — dump markdown (empty body if no entries)
- `curl -X DELETE http://192.168.0.44/api/crashlog` — truncate
- `lango> crashlog` / `lango> crashlog clear` — same via serial CLI
- `/api/file?name=crashlog` still works as a fallback

### NVS Namespaces
`wifi_config`, `llm_config`, `tg_config`, `stt_config`, `tts_config`, `proxy_config`, `search_config`

### SERVICES.md Hot-Reload Keys

`littlefs_data/config/SERVICES.md` is parsed at boot and on save via web UI (no restart needed). Key sections:

```
## LLM (Anthropic / OpenAI / OpenRouter)
api_key: <key>
provider: openrouter
model: openrouter/auto

## Local Model (Ollama)
base_url: http://192.168.0.51:11434/v1
api_key: ollama
model: qwen3:8b
voice_provider: openrouter        # LLM provider for voice channel (PTT/wake word)
voice_model: openai/gpt-4o-mini   # fast cloud model for low-latency voice responses
system_provider: openrouter       # LLM provider for system/heartbeat/cron channel
system_model: openai/gpt-4o-mini  # pinned model — avoids openrouter/auto content refusals

## Local Audio (mlx-audio)
base_url: http://192.168.0.51:8000
local_model: mlx-community/Kokoro-82M-bf16   # configurable TTS model
local_voice: af_heart                         # configurable TTS voice (af_heart, af_sky, am_adam…)
```

NVS values take priority over SERVICES.md on initial load; `services_config_reload()` overrides NVS unconditionally (called when file saved via web UI).

### Host-side mlx-audio setup (Mac at 192.168.0.51)

Served via `launchd` (`~/Library/LaunchAgents/com.mlx-audio.server.plist`). Env vars set in the plist: `PATH` must include `/opt/homebrew/bin` (ffmpeg discovery — Whisper decoding fails without it), `HF_HUB_OFFLINE=1` and `TRANSFORMERS_OFFLINE=1` (avoids spurious HF re-checks).

**Tokenizer workaround for `mlx-community/whisper-large-v3-turbo`**: that repo ships an incomplete tokenizer (no `vocab.json` / `merges.txt` / `special_tokens_map.json`, plus a `tokenizer_config.json` whose `extra_special_tokens` is an array rather than the dict format transformers ≥5.4 expects). Loading it silently yields `vocab_size=0`, and decode later crashes in `DecodingTask._step` with `[max] Cannot max reduce zero size array`. Fix: copy the tokenizer file set from `openai/whisper-large-v3-turbo` into both mlx-community snapshot dirs under `~/.cache/huggingface/hub/` and delete `.no_exist/` so HF rechecks. Also patched `mlx_audio/stt/models/whisper/whisper.py:176` (`non_speech_tokens`) to skip empty `encode()` results defensively — that crash shows up first as `IndexError: list index out of range`.

Symptom if this regresses: device log shows `[stt] STT failed in ~1s (HTTP 200) — falling back to cloud`. Confirmation command: `/Library/Frameworks/Python.framework/Versions/3.12/bin/python3 -c "from transformers import WhisperTokenizer; t=WhisperTokenizer.from_pretrained('mlx-community/whisper-large-v3-turbo'); print(t.vocab_size)"` should print `50257`, not `0`.

## OTA Notes

- Default device IP: `192.168.0.44` (fixed via DHCP reservation)
- OTA requires ≥22KB free SRAM (`OTA_MIN_FREE_HEAP`). If rejected with `low_heap`, reboot first: `curl -X POST http://192.168.0.44/api/reboot`
- Download speed ~10-20 KB/s over WiFi; 2MB firmware takes ~150-200s
- Script auto-retries once on error; detects successful reboot even if status broadcast is missed
- LittleFS changes (SERVICES.md etc.) are NOT flashed by OTA script — use web UI Save or serial flash at `0xa30000`

## TLS & Latency Optimization

**Persistent HTTP sessions** (`main/llm/http_session.c`) keep TLS connections alive across requests, avoiding ~300-500ms handshake overhead per call. Three persistent sessions are maintained:
- **STT** (Groq Whisper): 20s timeout, 4KB buffers
- **TTS** (Groq PlayAI): 20s timeout, 8KB RX buffer
- **LLM** (OpenRouter): 60s timeout, 4KB buffers — lazy-initialized on first cloud call

All sessions auto-retry on any error (reset + re-perform). Plaintext Ollama requests use fresh clients (no TLS to save).

**mbedTLS configuration** (`sdkconfig.defaults.esp32s3`):
- `EXTERNAL_MEM_ALLOC=y` — TLS buffers in PSRAM (not SRAM)
- `DYNAMIC_BUFFER=y` + `DYNAMIC_FREE_CONFIG_DATA=y` — buffers allocated only during active I/O
- `CLIENT_SSL_SESSION_TICKETS=y` + `ESP_TLS_CLIENT_SESSION_TICKETS=y` — session ticket resumption
- Input buffer 16KB, output buffer 4KB

**Timeout strategy**: per-I/O-operation timeouts, not total. Local LAN: 5s (STT), 15s (TTS). Cloud: 20s (STT/TTS), 60s (LLM). Local backoff after failure: 30s. VAD silence: 500ms.

**STT optimization**: `language=en` + `response_format=text` sent to Whisper — skips language detection (~0.5-1s) and reduces response size.

**Voice-mode LLM**: `max_tokens=400` (vs 4096 for text), `temperature=0.7`, `speed=1.1` on TTS.

## Weather Tool

`main/tools/tool_weather.c` — fetches from `wttr.in/?format=j1`. Response buffer is 20KB (j1 format returns 12-16KB JSON). Previously 8KB caused truncation and parse failures.

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
| 3 | I2S_BCLK | MAX98357A BCLK + INMP441 SCK (shared bus) |
| 4 | I2S_LRCLK | MAX98357A LRC + INMP441 WS (shared bus) |
| 6 | I2S_DOUT (TX) | MAX98357A DIN (speaker) |
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
SCK  → GPIO3   (shared I2S bus — same wire as MAX98357A BCLK)
WS   → GPIO4   (shared I2S bus — same wire as MAX98357A LRC)
L/R  → GND  (left channel)
```

## Adding a New Tool

1. Create `main/tools/tool_<name>.c` and `.h` following the pattern of any existing tool (e.g. `tool_weather.c`): implement `tool_<name>_execute(input_json, output, output_size)`.
2. Add `"tools/tool_<name>.c"` to `main/CMakeLists.txt` SRCS.
3. Register in `main/tools/tool_registry.c`: add schema JSON and a call to `tool_registry_register()`.

## OLED Display (SSD1306 128×64)

I2C at 0x3C on GPIO 9/10 (shared bus with camera SCCB + PCA9685). 1KB PSRAM framebuffer, 2Hz refresh task on Core 0.

**Idle screen** (LED state: READY):
- Row 0-15: Large HH:MM | abbreviated IP | `O+ A- F+` local service status (Ollama/mlx-audio/Apfel)
- Row 18: date | RSSI bars
- Row 30: **Rotating info line** (5s cycle): provider → next heartbeat task → slot 2 → rate limit
- Row 40-48: message preview or token counts + SRAM/PSRAM stats
- Row 56: uptime

**Active screen** (LED: THINKING/SPEAKING/LISTENING):
- Row 0: provider/model
- Row 12: `[WS] Thinking...  #14` — channel + status + daily message count
- Row 24-40: message preview (3 lines)
- Row 48: token counts
- Row 56: RSSI + uptime

**Thread-safe setter API** — any task pushes data, render task only reads static state:
- `oled_display_set_local_status(ollama, audio, apfel)` — from agent_loop after health check
- `oled_display_set_channel("WS")` — from agent_loop on message receive (also increments daily counter)
- `oled_display_set_rotate_line(slot, text)` — slot 0 auto-set by set_provider, slot 1 from heartbeat, slot 3 rate limit
- No cross-module function calls from the render task (prevents init-order crashes + stack issues)

Files: `main/display/oled_display.h/.c` (API + render), `main/display/ssd1306.h/.c` (I2C driver)

## Key Files

| File | Purpose |
|------|---------|
| `main/langoustine.c` | App entry point: init sequence, outbound dispatch task |
| `main/langoustine_config.h` | All `LANG_*` constants and `MIMI_*` compat aliases |
| `main/langoustine_secrets.h.example` | Template for build-time secrets |
| `main/agent/agent_loop.c` | Core AI ReAct loop (Core 1); channel-aware LLM routing; VOICE MODE injection |
| `main/agent/context_builder.c` | Assembles system prompt from SOUL/USER/MEMORY/skills |
| `main/gateway/ws_server.c` | HTTP+WebSocket server, all REST endpoints |
| `main/audio/audio_pipeline.c` | PSRAM ring buffer + STT task coordination; Opus bypass for local STT |
| `main/audio/stt_client.c` | Whisper multipart POST (Groq cloud + mlx-audio local); boot warmup task |
| `main/audio/tts_client.c` | PlayAI/Kokoro POST + PSRAM cache; local mlx-audio; configurable model/voice; boot warmup task |
| `main/audio/wake_word.c` | WakeNet9 wake word detection; VAD silence 700ms |
| `main/audio/i2s_audio.c` | I2S driver: simplex TX (I2S0 master) + RX (I2S1 slave) + WAV playback |
| `main/llm/llm_proxy.c` | LLM request routing; Ollama local health check (15s cache); boot warmup task |
| `main/telegram/telegram_bot.c` | Long-poll Telegram bot |
| `main/bus/message_bus.c` | FreeRTOS inbound/outbound queues |
| `main/config/services_config.c` | SERVICES.md parser (load + hot-reload) |
| `main/memory/psram_alloc.h` | PSRAM/SRAM allocation wrappers |
| `main/cli/serial_cli.c` | Full UART REPL with all config commands |
| `sdkconfig.defaults.esp32s3` | ESP32-S3 SDK defaults (flash, PSRAM, partitions) |
| `partitions_s3_32mb.csv` | Flash partition map |
| `littlefs_data/` | Static filesystem content (baked into littlefs.bin) |
| `littlefs_data/config/SERVICES.md` | Hot-reloadable service config (keys, URLs, voice settings) |
| `littlefs_data/HEARTBEAT.md` | Scheduled agent task checklist (30m health, daily briefing, surf, ARM stock) |
