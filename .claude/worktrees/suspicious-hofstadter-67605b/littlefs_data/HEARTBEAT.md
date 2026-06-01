# Heartbeat Tasks

Markers: `[daily HH:MM]` = once/day (Pacific time), `[daily DOW HH:MM]` = specific weekday, `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap. System channel — use `telegram_send_message` or `send_email` for notifications; NEVER use the `say` tool.

(The previous `[30m]` soak-logging task was removed 2026-05-01: every-30-min LLM ticks generated unreliable formatted numbers in /lfs/memory/soak.md — the LLM frequently forgot to divide by 1000 or mis-formatted units. The alarm threshold (SRAM<15K AND min<10K) never fired in practice, and the work cost ~$0.10/day in tokens for data nobody reads. Live state is available via /api/sysinfo, /api/crashlog, /api/coredump and the in-firmware wifi_recovery + ag_wdog watchdog.)

- [daily 22:00] Nightly check. Call system_info. Send telegram_send_message to chat_id 5538967144: "🌙 Nightly: uptime [X]h, heap [X] (min [X]), psram [X], rssi [X]dBm. [one-line all-clear or concern]." Use the live system_info values directly — do NOT format into k/MB units that need dividing; quote the raw "Free heap"/"Min free heap"/"Free PSRAM" numbers as bytes with thousands-separators.

- [daily Mon 06:50] Weekly ARM + PC digest. web_search "ARM Holdings news this week". web_search "PC Chromebook market news this week". send_email to: jcfield@gmail.com subject "📊 Weekly ARM + PC Intel — [date]". Top 3 from each in one sentence each, under 300 words, no markdown.

- [daily Sat 18:00] Weekend surf check for Sunday at Pacifica Linda Mar (beginner surfer — James). noaa_buoy station 46012 (metres→feet: 1m≈3.3ft; Linda Mar ≈60% of open-ocean buoy). web_search "Pacifica Linda Mar surf forecast Sunday [date]". Beginner rating: ✅GO 1-3ft clean · ⚠️MAYBE 3-4ft · ❌SKIP 4ft+. telegram_send_message to chat_id 5538967144: "🏄 Sunday Linda Mar: [Xft @ Xs period, Xkt wind dir]. ⭐X/5 — [GO/MAYBE/SKIP]. [One sentence reason]." Be honest — James trusts the verdict.
