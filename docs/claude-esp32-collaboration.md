# Claude ↔ ESP32 Collaboration Architecture

## Overview

Claude (running in Cowork) and the ESP32 (Lango) collaborate as a brain/IO split:

| Layer | Runs on | Responsibilities |
|-------|---------|-----------------|
| **Intelligence** | Claude (Cowork) | Morning briefing, ARM digest, Wirecutter deals, weekend planner, memory compaction, web search, content composition |
| **I/O** | ESP32 | Email send (SMTP), Telegram send/receive, local sensors (printer, HA, NOAA buoy), audio (I2S, wake word), Frame TV art, ARM stock snapshot |

This avoids duplicate messages — each piece of content has exactly one author and one sender.

## Relay Bridge

Because the Cowork sandbox is LAN-isolated (can't reach `192.168.0.44` directly), a small Mac relay daemon bridges the gap:

```
Claude writes ──► ~/Lango/outbox.json ──► lango_relay.py ──► POST /api/relay ──► ESP32 sends
                  (Mac filesystem)         (Mac daemon,          (new firmware
                                           polls every 5s)        endpoint)
```

### New firmware endpoint: `POST /api/relay`

Added in `main/gateway/ws_server.c`. Accepts:

```json
// Send email via SMTP credentials from SERVICES.md
{"type": "email", "to": "jcfield@gmail.com", "subject": "...", "body": "..."}

// Send Telegram message
{"type": "telegram", "chat_id": "5538967144", "text": "..."}
```

Returns `{"ok": true}` immediately; send runs async (16KB task stack for SMTP TLS).
Auth: same Bearer token as all other `/api/*` endpoints.

### Scripts

| Script | Purpose |
|--------|---------|
| `scripts/lango_relay.py` | Mac daemon — polls outbox.json, POSTs to /api/relay |
| `scripts/com.lango.relay.plist` | launchd agent — keeps relay running at login |
| `scripts/lango_send.py` | CLI helper — Claude uses this from Cowork bash to queue outbox items |

### Install the relay daemon

```bash
# Copy plist to LaunchAgents
cp ~/Lango/scripts/com.lango.relay.plist ~/Library/LaunchAgents/

# Load it (starts immediately, persists across reboots)
launchctl load ~/Library/LaunchAgents/com.lango.relay.plist

# Check it's running
launchctl list | grep lango

# View logs
tail -f ~/Library/Logs/lango_relay.log
```

### How Claude sends a message

From a Cowork bash tool:

```bash
# Email
python3 ~/Lango/scripts/lango_send.py email "Subject" "Body text"

# Telegram
python3 ~/Lango/scripts/lango_send.py telegram "Message text"
```

The relay daemon picks it up within 5 seconds and POSTs to the device.

## Division of Responsibilities

### Claude owns (Cowork scheduled tasks)

| Task | Schedule | Output |
|------|----------|--------|
| Morning briefing | Daily 06:20 | Email via relay |
| Weekly ARM + PC digest | Monday 06:50 (HEARTBEAT.md) | Email via relay |
| Wirecutter deals | Monday morning | Email via relay |
| Weekend planner | Friday | Telegram via relay |
| Memory compaction | Sunday | Writes MEMORY.md |

### ESP32 owns (cron.json / HEARTBEAT.md)

| Task | Schedule | Notes |
|------|----------|-------|
| `prefetch` | Every 4h | Gathers weather/markets/printer → brief_data.md |
| `armpre01` | Daily | ARM stock snapshot → arm_stock_today.md |
| `surf0002/0003` | Sat/Fri | NOAA buoy → Telegram verdict |
| `haupd001` | Daily | HA update check |
| `kupd0001` | Daily | Klipper update check |
| `tvart001` | Daily | Frame TV art generation |
| `cmpct006` | Sunday | Memory compaction (can move to Claude) |
| Nightly check | 22:00 (HEARTBEAT.md) | sysinfo → Telegram |
| Telegram polling | Continuous | Receive and respond to messages |

### Disabled ESP32 crons (Claude took over)

- `brief001` — Morning Briefing
- `wire0003` — Monday Wirecutter Deals  
- `armnw005` — Sunday ARM Ecosystem News
- `wknd0004` — Friday Weekend Planner

## Rebuild & Flash

After adding `/api/relay` to `ws_server.c`:

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/tty.usbserial-* flash
```

No LittleFS reflash needed — `cron.json` changes should be pushed via the web UI
(`POST /api/file?name=cron`) or by flashing `littlefs.bin`.

To push the updated cron.json without a full flash:

```bash
curl -X POST "http://192.168.0.44/api/file?name=cron" \
  --data-binary @littlefs_data/cron.json
```
