# Langoustine — Remaining Work

Generated from code review + coredump analysis. Items ordered by priority.

---

## 🟡 P1 — Feature test checklist

Run these to confirm the current build is solid.

### Voice pipeline — wake word

1. Say "Hi ESP" from ~1m away, normal speaking volume
2. LED → white (listening)
3. Say a short question ("what time is it?")
4. LED → blue (thinking) → cyan (TTS) → green (idle)
5. **Check:** transcript appears in dev console monitor feed
6. **Check:** LLM response text in monitor feed
7. **Check:** TTS WAV cached, `tts_id` sent to browser
8. **Check:** I2S speaker plays response (`LANG_I2S_AUDIO_ENABLED=1` ✅)
9. **Check:** no false wake during TTS playback

### Voice pipeline — PTT (BOOT button + UAC mic)

1. Plug webcam into USB-A port, wait for `UAC device connected` in serial log
2. Hold BOOT button (GPIO0) — LED → white
3. Speak into webcam mic, release button
4. Same LED/monitor flow as wake word path
5. **Check:** Opus encoding log line shows compression ratio

### Web UI

1. Navigate to `http://192.168.0.44` — no cert warning
2. Type a message, press Enter — tokens stream
3. Dev console at `http://192.168.0.44/console` — monitor feed live
4. **Check:** `ws:` WebSocket connects (browser console, no mixed-content errors)

### Frame TV tool

1. Say/type "put an impressionist sunset on the Frame"
2. **Check:** agent calls `frame_tv` tool, responds with "generating..."
3. **Check:** Nanoframe app receives POST at port 11436
4. **Check:** image appears on TV after ~30–60 s

### Telegram

1. Send a message to the bot
2. **Check:** placeholder "🤔 thinking..." appears
3. **Check:** response edits placeholder in-place
4. Test a tool-using prompt: "what's the weather in SF?" (web_search + get_weather)
5. **Check:** cron briefing delivered at expected time

### Cron jobs

All 7 jobs have `chat_id: 5538967144`. Verify skip-stale guard advanced overdue
`next_run` values on boot (check serial log for `cron: skipping stale job`).
Manually trigger: `lango> heartbeat_trigger` — should not fire cron, only heartbeat.
Watch next natural cron fire (daily briefing) and confirm Telegram delivery.

### OLED display

| Scenario | Expected |
|----------|----------|
| Idle | time, date, provider, SRAM/PSRAM, uptime |
| LLM thinking | provider/model line, status = thinking |
| TTS active | status = speaking |
| STT active | status = transcribing |
| Tool call | tool name shown briefly |

### LED states

| Trigger | Expected color |
|---------|---------------|
| Boot | solid red |
| WiFi connecting | yellow blink |
| Idle/ready | breathing green |
| Wake word / PTT | pulsing white |
| LLM thinking | pulsing blue |
| TTS generating | pulsing cyan |
| Error (e.g. STT fail) | fast red flash → reverts to green |

### Agent tools — smoke test

```
"What time is it in Tokyo?"            → get_current_time
"Search for latest Arm news"           → web_search
"What's the weather in San Francisco?" → get_weather
"Read my MEMORY.md"                    → read_file
"What's the temperature of the ESP?"   → device_temp
"Put a painting of the ocean on the Frame" → frame_tv
```

Check each produces a clean response with no stack overflow / WDT reset.

### Local STT / TTS / LLM (if mlx-audio / Ollama running)

1. Set `base_url: http://<mac-ip>:8000` in SERVICES.md → save
2. Trigger voice request — monitor feed should show "Trying local STT..."
3. On success: "Local STT success" in monitor feed
4. On failure: "Local STT offline, falling back to cloud" + 60s backoff
5. Same test for local TTS (`base_url` serving `/v1/audio/speech`)
6. For local LLM: `lango> set_local_url http://<mac-ip>:11434/v1` → voice request → monitor shows "routing: local (ollama)"

---

## 🟠 P2 — Fixes & features

### 1. TTS truncation cuts mid-sentence

`main/agent/agent_loop.c` ~line 617 — truncates at 80 chars hard.
Walk back to last sentence boundary (`.`, `!`, `?`) before the limit.
Consider raising limit to 200 chars now that power is stable.

### 2. stt_task stack watermark logging

Add after each STT transcription in `audio_pipeline.c`:
```c
ESP_LOGI(TAG, "stt_task stack HWM: %u bytes free",
         uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
```
Confirm 24 KB stack has ≥4 KB headroom in practice. If HWM < 4 KB, raise to 26 KB.

### 3. Persistent local HTTP session for STT

`main/audio/stt_client.c` — `stt_transcribe_local()` creates a fresh
`esp_http_client` on each call (stack-heavy). Add a `static http_session_t
s_local_session` (plain TCP, no TLS) initialized on first use. This reduces
per-call stack depth by ~4 KB and allows stt_task to drop back to 20 KB,
reclaiming 4 KB SRAM.

### 4. `/api/message` POST endpoint

For external callers (Claude, scripts) to trigger agent turns over HTTP without
a browser or Telegram. Auth via Bearer token. Returns 202 immediately, response
goes to specified channel.

### 5. Mac-side credential proxy (NanoClaw-inspired)

Move API keys (Anthropic/OpenRouter/Groq) off the ESP32 NVS into a Mac-side
proxy that injects credentials at request time. ESP32 authenticates to the
proxy with a local LAN secret only. `tool_frame_tv` already follows this
pattern (ESP32 → Mac → cloud). Extends it to LLM/STT/TTS calls.

**Why:** NVS keys survive if the device is stolen/cloned. A proxy means cloud
keys never leave the Mac. Also enables per-request rate limiting and logging.

---

## 🔵 P3 — Nice to have

### 6. Weekly memory compaction cron

Already in `cron.json` as job `cmpct006` (system channel, weekly). Confirm it
fires and actually shrinks MEMORY.md when it runs.

### 7. Context bloat reduction (PICO_AGENT direction)

`agent_loop.c` + `context_builder.c` — current system prompt is 8–15K tokens
on complex turns, ballooning to 350K+ on multi-tool heartbeat runs. See
`docs/PICO_AGENT.md` for the full design. Short-term: cap `MEMORY.md` retrieval
to the top-N most relevant entries rather than the full file.

---

## ✅ Already done

- **TLS → plain HTTP** — `httpd_start()` on port 80, no `esp_https_server`, no embedded certs. HTML auto-detects `ws:`/`wss:` from `location.protocol`. Stale "HTTPS" log message fixed.
- **`LANG_MAX_TOOL_CALLS` 2 → 4** — raised after TLS removal freed ~25 KB SRAM headroom
- **`LANG_I2S_AUDIO_ENABLED` = 1** — speaker playback active
- **Samsung Frame TV tool** — `tool_frame_tv`: prompt → Nanoframe Mac app (port 11436) → DALL-E 3 → TV
- **stt_task stack overflow** (16 KB → 24 KB) — coredump confirmed, flashed
- **Bootloader boot loop after OTA** — full reflash with partition table
- **All cron jobs `chat_id: 5538967144`**
- **OLED layout** (IP + RSSI, date row)
- **`tool_get_time`** uses NTP directly
- **TTS voice** `autumn` (was `tara`)
- **SERVICES.md** hot-reload on save
- **Klipper Moonraker URL** port `:7125`
- **Default model** `openrouter/auto`
- **Briefing prefetch pattern** (cron pre-fetches to `brief_data.md`)
- **Log rotation hardening** (300 KB cap, 8 gens, PID lock — fixed 159 GB `/tmp` blowup)
- **Klipper daily probe** (silent when current)
- **Monday ARM digest** rewritten to use structured email with sources
