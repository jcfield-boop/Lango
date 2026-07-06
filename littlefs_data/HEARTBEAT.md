# Heartbeat Tasks

Markers: `[daily HH:MM]` = once/day (Pacific time), `[daily DOW HH:MM]` = specific weekday, `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap. System channel — use `telegram_send_message` or `send_email` for notifications; NEVER use the `say` tool.

(The previous `[30m]` soak-logging task was removed 2026-05-01: every-30-min LLM ticks generated unreliable formatted numbers in /lfs/memory/soak.md — the LLM frequently forgot to divide by 1000 or mis-formatted units. The alarm threshold (SRAM<15K AND min<10K) never fired in practice, and the work cost ~$0.10/day in tokens for data nobody reads. Live state is available via /api/sysinfo, /api/crashlog, /api/coredump and the in-firmware wifi_recovery + ag_wdog watchdog.)

- [daily 22:00] Nightly check. Call system_info. Send telegram_send_message to chat_id 5538967144: "🌙 Nightly: uptime [X]h, heap [X] (min [X]), psram [X], rssi [X]dBm. [one-line all-clear or concern]." Use the live system_info values directly — do NOT format into k/MB units that need dividing; quote the raw "Free heap"/"Min free heap"/"Free PSRAM" numbers as bytes with thousands-separators.

