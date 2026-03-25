# Langoustine — Remaining Work

Generated from code review. Items ordered by priority.

---

## 🔴 P0 — Broken right now

### 0. stt_task stack overflow → system crash (CONFIRMED via coredump)

**Root cause (coredump decoded 2026-03-25):**

```
Crashed task: wifi  exccause: 0x1c (LoadProhibitedCause)
a2 = 0xa5a5a5a5  ← FreeRTOS stack fill pattern (overflow signature)
stt_task: STACK USED/FREE = 18464/2084   (task size = 16384 bytes)
```

The `stt_task` overflows by ~2KB. Overflow corrupts the adjacent `wifi` task TCB,
causing the FreeRTOS scheduler to dereference `0xa5a5a5a5` and fault.

**Why it overflows:** When local STT is configured, `stt_transcribe_local()` creates
a *new* `esp_http_client` (not the persistent session) and calls `perform()` inside
the stt_task stack frame, on top of the `opus_encode_pcm_to_ogg()` call that already
consumed significant stack. Combined depth exceeds 16KB.

**Fix: increase stt_task stack from 16KB → 28KB SRAM.**

File: `main/audio/audio_pipeline.c`, line 176:
```c
// Before:
16 * 1024, NULL,
// After:
28 * 1024, NULL,
```

Add a matching constant to `main/langoustine_config.h`:
```c
#define LANG_STT_STACK_SIZE   (28 * 1024)
```

And use it in `audio_pipeline.c`:
```c
LANG_STT_STACK_SIZE, NULL,
```

**Bonus fix (local STT stack pressure):** `stt_transcribe_local()` uses
`esp_http_client_init()` + `esp_http_client_perform()` + cleanup on each call —
a fresh TLS-capable client even for plain HTTP. Consider adding a persistent
`http_session_t s_local_session` for the local path (same pattern as the cloud
session), initialized with `transport_type = HTTP_TRANSPORT_OVER_TCP` (no TLS).
This reduces per-call stack depth and avoids TCP handshake overhead on LAN.
Not required for the crash fix — stack increase alone is sufficient.

---

### 1. cron.json: all telegram jobs missing `chat_id`

**File:** `littlefs_data/cron.json`

`cron_sanitize_destination()` in `cron_service.c` (lines 38–45) silently
downgrades any job where `channel == "telegram"` and `chat_id` is empty or
`"cron"` back to `channel: "system", chat_id: "cron"`. All 6 jobs in the
current `cron.json` have `"channel": "telegram"` but no `"chat_id"` field,
so every one of them will fire on the system channel, not Telegram.

**Fix:** Add `"chat_id": "<YOUR_TELEGRAM_NUMERIC_ID>"` to each job in
`cron.json`. James can find his numeric chat ID by messaging `@userinfobot`
on Telegram — it replies with the numeric ID (e.g. `"123456789"`).

```json
{
  "id": "brfng001",
  "channel": "telegram",
  "chat_id": "123456789",   ← add this to every job
  ...
}
```

---

## 🟡 P1 — Behaviour issues

### 2. TTS truncation cuts mid-sentence

**File:** `main/agent/agent_loop.c` ~line 617

```c
if (strlen(final_text) > 80) {
    strncpy(tts_buf, final_text, 80);
    tts_buf[80] = '\0';
```

This hard-truncates at 80 characters regardless of sentence boundaries,
producing clipped, unnatural speech. The limit exists to keep WAV size under
~200KB on USB-powered supplies, which is a valid concern.

**Fix:** Truncate at the last sentence boundary (`.`, `!`, `?`) before the
80-char limit, falling back to the last space if no punctuation is found.
Something like:

```c
#define TTS_MAX_CHARS 80
if (strlen(final_text) > TTS_MAX_CHARS) {
    strncpy(tts_buf, final_text, TTS_MAX_CHARS);
    tts_buf[TTS_MAX_CHARS] = '\0';
    /* Walk back to last sentence-ending punctuation */
    int cut = TTS_MAX_CHARS - 1;
    while (cut > 20 && tts_buf[cut] != '.' &&
           tts_buf[cut] != '!' && tts_buf[cut] != '?') {
        cut--;
    }
    if (cut > 20) tts_buf[cut + 1] = '\0';
    tts_text = tts_buf;
}
```

### 3. Memory compactor not on a schedule

**Files:** `littlefs_data/skills/memory-compactor.md`, `littlefs_data/cron.json`

`USER.md` notes "missing memory compaction behavior" as a past lesson. The
`memory-compactor.md` skill exists but there is no cron job that runs it.
`MEMORY.md` is currently small but will grow; without compaction the LLM
context will bloat and eventually hit `LANG_MEMORY_MAX_BYTES` (16KB).

**Fix:** Add a weekly cron job (e.g. Sunday 2 AM) to run the compactor:

```json
{
  "id": "cmpct006",
  "name": "Weekly Memory Compaction",
  "enabled": true,
  "kind": "every",
  "interval_s": 604800,
  "next_run": 0,
  "last_run": 0,
  "delete_after_run": false,
  "channel": "system",
  "chat_id": "cron",
  "message": "Run the memory-compactor skill: read MEMORY.md, remove duplicate or superseded entries, merge related facts, keep total size under 8KB. Write the compacted result back to MEMORY.md. Log a one-line summary of what was removed."
}
```

Note: `system` channel is correct here — no user reply needed.

---

## 🟠 P2 — External integration (Claude ↔ Lango)

### 4. Add `/api/message` POST endpoint

**File:** `main/gateway/ws_server.c`

This is the key missing piece for Claude (and other external callers) to
trigger Lango agent turns over HTTPS without needing a browser or Telegram.
The existing `s_auth_token` / `LANG_SECRET_HTTP_TOKEN` infrastructure is
already in place for auth.

**Endpoint spec:**

```
POST /api/message
Authorization: Bearer <LANG_SECRET_HTTP_TOKEN>
Content-Type: application/json

{
  "message": "Check the ARM stock price",
  "channel": "system",        // optional, default "system"
  "chat_id": "api"            // optional, default "api"
}
```

**Behaviour:**
- Validate Bearer token against `s_auth_token` (same check used by other
  protected endpoints).
- Push a `mimi_msg_t` onto the inbound message bus with the given channel,
  chat_id, and message content.
- Return `{"status": "queued"}` immediately (202 Accepted) — don't block
  waiting for the agent to respond, as LLM calls can take 10–30s and the
  HTTP server has a short request timeout.
- Responses will be routed to the channel specified (system → monitor
  broadcast; websocket → connected client; telegram → Telegram chat).

For a synchronous version (Claude waits for the response), a separate
`/api/ask` endpoint with a response queue/semaphore pattern could be added
later, but the async version is sufficient for orchestrated use.

**Why this matters:** Once this endpoint exists and Lango is reachable via a
tunnel (e.g. Cloudflare), Claude can:
- Trigger morning briefings, surf checks, or stock lookups on demand
- Execute GPIO actions and HA commands remotely
- Use Lango as a local sensor/actuator layer

---

## 🔵 P3 — Configuration / tuning

### 5. `LANG_MAX_TOOL_CALLS` = 2

**File:** `main/langoustine_config.h` line 52

The parallel tool execution path (`build_tool_results()`) allocates
`tool_exec_ctx_t ctxs[LANG_MAX_TOOL_CALLS]` which is currently 2. The LLM
can only request 2 tools in a single response. Raising this to 4 would let
compound briefing tasks (e.g. web_search + noaa_buoy + get_weather +
rss_fetch) run in parallel in a single turn rather than requiring 2
iterations.

**Constraint:** Each parallel task needs ~28KB SRAM stack + heap; the guard
at `free_sram < n * 28KB + 28KB` will fall back to sequential if memory is
tight. Raising the cap is low-risk — it only affects turns where the LLM
actually requests that many tools simultaneously.

**Suggested value:** `4`

### 6. Local speaker playback

**File:** `main/langoustine_config.h` line 134

```c
#define LANG_I2S_AUDIO_ENABLED  0
```

The MAX98357A amplifier is wired and `i2s_audio.c` + `tts_client.c` fully
support local playback — it's just disabled. The comment in `agent_loop.c`
says "keep WAV < ~200KB on USB-powered supplies."

**Decision needed:** If Lango has a stable 5V supply (which it does — the
wiring guide routes amp VIN to the USB 5V rail), enabling this would make
Lango a true voice assistant that speaks responses without needing a browser
tab open. The 80-char TTS limit (item 2 above) should be fixed first.

To enable: set `LANG_I2S_AUDIO_ENABLED 1` and flash.

---

## ✅ Already fixed (for reference)

- `littlefs_data/config/SERVICES.md` added to `.gitignore`
- All cron jobs switched from `websocket` to `telegram` channel
- `device_temp` tool description corrected from "ESP32-C6" to "ESP32-S3"
- mDNS service registration updated to `_https` on port 443
