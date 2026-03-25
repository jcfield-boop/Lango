# Langoustine — Remaining Work

Generated from code review + coredump analysis. Items ordered by priority.

---

## 🔴 P0 — Do this next

### 1. Drop TLS → plain HTTP/WS

**Why:** Browser mic (`getUserMedia`) was the only reason for HTTPS. With the
physical INMP441/UAC mic + wake word, the browser UI is text + status only.
Plain HTTP on port 80 frees ~20–30 KB SRAM per TLS session and eliminates
self-signed cert warnings. Current idle SRAM is ~27 KB — every TLS connection
eats into agent headroom.

**Files to change:**
- `main/gateway/ws_server.c`:
  - Replace `esp_https_server` with plain `httpd_start()` on port 80
  - Remove `esp_https_server.h` include and cert extern declarations
  - Remove the port-80 redirect server (it becomes the main server)
  - `ws_server_stop()`: call `httpd_stop()` instead of `httpd_ssl_stop()`
- `main/langoustine_config.h`: remove any HTTPS/cert-related constants
- `main/CMakeLists.txt`: remove `EMBED_TXTFILES` for cert/key PEM files
- `littlefs_data/console/index.html`: change `wss:` → `ws:`, `https:` → `http:`
- `littlefs_data/console/dev.html`: same WS URL fix

**After:** Rebuild + full flash. SRAM free should rise to ~47–57 KB at idle,
giving comfortable headroom for agent + TLS-free WebSocket sessions.

---

## 🟡 P1 — Feature test checklist

Run these in order after dropping TLS. Each is a go/no-go gate.

### Voice pipeline — wake word

1. Say "Hi ESP" from ~1m away, normal speaking volume
2. LED → white (listening)
3. Say a short question ("what time is it?")
4. LED → blue (thinking) → cyan (TTS) → green (idle)
5. **Check:** transcript appears in dev console monitor feed
6. **Check:** LLM response text in monitor feed
7. **Check:** TTS WAV cached, `tts_id` sent to browser
8. **Check:** I2S speaker plays response (requires `LANG_I2S_AUDIO_ENABLED=1` — see P2 #1)
9. **Check:** no false wake during TTS playback

### Voice pipeline — PTT (BOOT button + UAC mic)

1. Plug webcam into USB-A port, wait for `UAC device connected` in serial log
2. Hold BOOT button (GPIO0) — LED → white
3. Speak into webcam mic, release button
4. Same LED/monitor flow as wake word path
5. **Check:** Opus encoding log line shows compression ratio

### Web UI (after HTTP switch)

1. Navigate to `http://192.168.0.44` — no cert warning
2. Type a message, press Enter — tokens stream
3. Dev console at `http://192.168.0.44/console` — monitor feed live
4. **Check:** `ws:` WebSocket connects (browser console, no mixed-content errors)

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
"What time is it in Tokyo?"          → get_current_time
"Search for latest Arm news"         → web_search
"What's the weather in San Francisco?" → get_weather
"Read my MEMORY.md"                  → read_file
"What's the temperature of the ESP?" → device_temp
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

### 1. Enable I2S speaker playback

`main/langoustine_config.h` line ~134:
```c
#define LANG_I2S_AUDIO_ENABLED  1   // was 0
```
Hardware is wired (MAX98357A VIN→5V, SD→GPIO42). Fix TTS truncation (#2) first —
a 80-char limit means only the first sentence gets spoken.

### 2. TTS truncation cuts mid-sentence

`main/agent/agent_loop.c` ~line 617 — truncates at 80 chars hard.
Walk back to last sentence boundary (`.`, `!`, `?`) before the limit.
Consider raising limit to 200 chars now that power is stable.

### 3. stt_task stack watermark logging

Add after each STT transcription in `audio_pipeline.c`:
```c
ESP_LOGI(TAG, "stt_task stack HWM: %u bytes free",
         uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
```
Confirm 24 KB stack has ≥4 KB headroom in practice. If HWM < 4 KB, raise to 26 KB.

### 4. Persistent local HTTP session for STT

`main/audio/stt_client.c` — `stt_transcribe_local()` creates a fresh
`esp_http_client` on each call (stack-heavy). Add a `static http_session_t
s_local_session` (plain TCP, no TLS) initialized on first use. This reduces
per-call stack depth by ~4 KB and allows stt_task to drop back to 20 KB,
reclaiming 4 KB SRAM.

### 5. `/api/message` POST endpoint

For external callers (Claude, scripts) to trigger agent turns over HTTP without
a browser or Telegram. Auth via Bearer token. Returns 202 immediately, response
goes to specified channel. See original spec in prior PLAN.md.

---

## 🔵 P3 — Nice to have

### 6. `LANG_MAX_TOOL_CALLS` = 2 → 4

Low-risk — parallel spawn only fires if SRAM guard passes. After TLS removal
(~47 KB free idle), the guard will allow 2 parallel tools again. Raising cap
to 4 allows compound briefing tasks in one LLM turn.

### 7. Weekly memory compaction cron

Already in `cron.json` as job `cmpct006` (system channel, weekly). Confirm it
fires and actually shrinks MEMORY.md when it runs.

---

## ✅ Already fixed

- stt_task stack overflow (16 KB → 24 KB) — coredump confirmed, flashed
- SRAM/TLS headroom (28 KB → 24 KB stack, TLS sessions now succeed)
- Bootloader boot loop after OTA slot reset — full reflash with partition table
- All cron jobs have `chat_id: 5538967144` — PLAN item #1 resolved
- OLED layout (IP + RSSI, date row)
- `tool_get_time` uses NTP directly
- TTS voice: `autumn` (was `tara`)
- SERVICES.md hot-reload on save
- Klipper Moonraker URL port `:7125`
- Default model: `openrouter/auto`
