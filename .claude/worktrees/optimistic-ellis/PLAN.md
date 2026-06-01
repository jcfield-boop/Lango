# Langoustine — Remaining Work

Generated from code review + coredump analysis. Items ordered by priority.

---

## 🔴 P0 — Unblock USB serial before testing

### USB serial driver needs macOS Security approval (CH340)

**Status:** BLOCKED until user approves the WCH driver in System Settings.

**Symptoms:**
- `ls /dev/cu.*` shows only `Bluetooth-Incoming-Port` and `debug-console`
- `systemextensionsctl list` shows `cn.wch.CH34xVCPDriver [activated waiting for user]`
- SiLabs CP210x driver is active — if board used CP210x instead of CH340, it would work now
- HTTP server on device is hung (TCP connects, no response) — power cycle needed after driver fix

**To fix (user action required):**
1. System Settings → Privacy & Security → scroll to Security section
2. Click "Allow" next to "Software from WCH" (or similar CH340 entry)
3. Restart Mac (macOS requires reboot for kext approval)
4. After reboot: reconnect USB cable — `ls /dev/cu.usbserial-*` should show the port
5. Power-cycle the ESP32 (unplug/replug USB or use reset button) — HTTP server is hung

**After fix, use:**
```bash
ls /dev/cu.usbserial-*                            # find port
idf.py -p /dev/cu.usbserial-XXXX flash monitor   # flash + watch serial log
```

**Build is ready:** `build/langoustine.bin` (2.16MB) is already built. No need to rebuild.

**LittleFS trim for serial flash:**
```bash
python3 -c "d=open('build/littlefs.bin','rb').read(); last=[i for i,b in enumerate(d) if b!=0xFF][-1]; t=((last+1)+0xFFFF)&~0xFFFF; open('/tmp/littlefs_trim.bin','wb').write(d[:t])"
# Then flash:
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-XXXX -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x20000 build/langoustine.bin 0xa30000 /tmp/littlefs_trim.bin
```

---

## 🟡 P1 — Feature test checklist (run after driver fix + flash)

These tests verify all major subsystems. Run in order — each is a go/no-go gate.
Watch serial monitor output (`idf.py monitor`) during all tests.

### 0. Boot + SRAM check (serial log)

After flash + reset, in serial monitor:
- [ ] `SRAM free at boot: NNN bytes` — expect ≥80KB (was 89KB before latest build)
- [ ] `stt_warmup_task` fires at ~8s
- [ ] `tts_warmup_task` fires at ~18s
- [ ] `llm_warmup_task` fires at ~25s
- [ ] No stack overflow or assertion failures in first 60s

### 1. Web UI — plain HTTP, no cert warning

1. `http://192.168.0.44` loads without cert warning
2. Type "hello" → press Enter
3. **Check serial:** `agent_loop: received message` log
4. **Check browser:** response text appears immediately (before TTS)
5. **Check browser:** `▶ Listen` button appears after ~2-30s (TTS async)
6. **Check serial:** `stt_task stack HWM: N bytes free` — expect ≥4096

### 2. Tool calls — single and parallel

In browser, type: `"Search for latest ESP32 news and tell me the current time in Tokyo"`

- [ ] Agent calls `web_search` and `get_current_time` (should run sequentially or in parallel)
- [ ] Serial shows `tool par: SRAM NNNNN B < NNNNN needed` or parallel spawn log
- [ ] Response includes both search result and time
- [ ] SRAM stays ≥20KB during tool execution (check serial logs)

In browser, type a complex query to test up to 4 tools: `"What's the weather in SF, search for ARM Cortex-M news, what time is it in London, and what's the ESP32 temperature?"`

- [ ] Agent may request all 4 tools in one turn (LANG_MAX_TOOL_CALLS=4 now)
- [ ] No WDT reset, no stack overflow

### 3. TTS async flow

1. Send a message that generates a long response
2. **Check:** text appears in browser bubble BEFORE audio button
3. **Check:** `▶ Listen` button appears as follow-up
4. **Check serial:** `Sent text to browser (TTS pending)` then `tts_ready sent`
5. Click `▶ Listen` → browser plays audio
6. **Check serial:** `tts_task stack HWM: N bytes free` (if TTS logging added)

### 4. Voice pipeline — wake word

1. Say "Hi ESP" from ~1m away
2. **Check LED:** white pulse (listening)
3. Ask a short question
4. **Check serial:** STT result appears
5. **Check LED:** blue (thinking) → cyan (TTS) → green (idle)
6. **Check:** I2S speaker plays response (MAX98357A wired to GPIO3/4/6)
7. **Check serial:** `stt_task stack HWM: N bytes free` — expect ≥4096

### 5. Voice pipeline — PTT (UAC mic)

1. Plug webcam into USB-A port
2. **Check serial:** `UAC device connected` within 3s
3. Hold BOOT button (GPIO0) — **Check LED:** white
4. Speak, release button
5. **Check serial:** `Opus encoding: N → M bytes` log
6. **Check:** same LED flow and I2S playback as wake word

### 6. Telegram

1. Send message to bot
2. **Check:** "🤔 thinking..." placeholder appears
3. **Check:** placeholder edits to response in-place
4. Test: `"What's the weather in SF?"` → verify tool use + response
5. **Check:** cron briefing fires at 6:05 AM PST (chat_id 5538967144)

### 7. Cron jobs

After boot, check serial for:
- `cron: skipping stale job` — overdue jobs (if any) should auto-advance
- At 6:05 AM PST: `brfng001` fires → Telegram message to 5538967144

Manual trigger (serial CLI): `lango> heartbeat_trigger`

### 8. OLED display

| Scenario | Expected |
|----------|----------|
| Idle | HH:MM / IP / O+A- status / date / uptime |
| LLM thinking | provider/model, `[WS] Thinking... #N` |
| TTS active | status = speaking |
| STT active | status = transcribing |
| Tool call | tool name visible |

### 9. LED states

| Trigger | Expected |
|---------|----------|
| Boot | solid red |
| WiFi connecting | yellow blink |
| Idle/ready | breathing green |
| Wake word / PTT | pulsing white |
| LLM thinking | pulsing blue |
| TTS generating | pulsing cyan |
| Error | fast red flash → green |

### 10. Local pipeline (if mlx-audio + Ollama running on 192.168.0.51)

1. Confirm SERVICES.md has `base_url: http://192.168.0.51:8000`
2. Voice request → **serial:** `Trying local STT...` → `Local STT success`
3. Text request → **serial:** `routing: local (ollama)`
4. Simulate offline: stop mlx-audio → **serial:** `Local STT offline, fallback`

---

## 🟠 P2 — Watch + fix after P1

### 1. STT stack headroom

Check serial after wake word: `stt_task stack HWM: N bytes free`
- If N < 4096: raise stack from 24KB → 26KB in `audio_pipeline.c`

### 2. HTTP server hang (already observed)

Device's httpd was unresponsive on 2026-04-07. Symptom: TCP connects to :80
but never returns HTTP response. Possible culprit: `s_clients_lock` held by
a task that died, or httpd task stack overflow (8KB).

To diagnose next occurrence: check serial for any `Guru Meditation Error` or
`***ERROR*** A stack overflow in task httpd` before the hang.
Consider raising `cfg.stack_size` from 8192 → 10240 as a precaution.

---

## 🔵 P3 — Nice to have

### 1. Weekly memory compaction cron

Confirm job `cmpct006` fires and shrinks `/lfs/memory/MEMORY.md` when it runs.

### 2. Browser TTS pending indicator

Show "generating audio..." between the `message` and `tts_ready` events.
The last assistant bubble could show a subtle spinner for 2-40s while Kokoro runs.

### 3. httpd stack investigation

If HTTP hang recurs, add WDT notification logging:
```c
cfg.stack_size = 10240;  // bump from 8192
```

---

## ✅ Already complete

- **P0: Drop TLS** — `httpd_start()` on port 80, no `esp_https_server`
- stt_task stack overflow fix (16 KB → 24 KB)
- SRAM explosion fixed (7KB → 89KB free): 5 task stacks moved to PSRAM
- All cron jobs have `chat_id: 5538967144`
- OLED layout (IP + RSSI, date row, rotating info line)
- `tool_get_time` uses NTP directly
- TTS voice: `autumn` (was `tara`)
- SERVICES.md hot-reload on save
- Klipper Moonraker URL port `:7125`
- Default model: `openrouter/auto`
- TTS truncation fixed: 1500 chars with sentence-boundary walk-back
- I2S speaker enabled: `LANG_I2S_AUDIO_ENABLED = 1`
- `/api/message` POST endpoint (auth via Bearer token, 202 async)
- Persistent local HTTP session for STT (`s_session` in `stt_client.c`)
- TTS async: text sent immediately, `tts_ready` follow-up with audio ID
- STT stack watermark logging (`audio_pipeline.c` after transcription)
- `LANG_MAX_TOOL_CALLS` raised 2 → 4 (SRAM guard prevents parallel overrun)
- Build confirmed good: `build/langoustine.bin` 2.16MB, 48% of 4MB slot
