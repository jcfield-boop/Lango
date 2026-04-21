# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[daily DOW HH:MM]` = specific weekday, `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap. System channel — use `telegram_send_message` or `send_email` for notifications; NEVER use the `say` tool.

- [30m] Call system_info (use HH:MM from the heartbeat check time above). Then: (1) Write ONE line append=true to /lfs/memory/soak.md using write_file. Exact format: `"HH:MM — heap=Xk min=Xk psram=X.XMB rssi=XdBm up=Xm"` — example: `"14:30 — heap=12420k min=12100k psram=12.5MB rssi=-41dBm up=60m"`. Use "Free heap" for heap/min in whole kB; PSRAM for psram with one decimal; rssi no space before "dBm"; uptime in whole minutes. (2) Check "SRAM free" from system_info: ONLY if SRAM free < 15000 AND min heap < 10000, send one telegram_send_message "⚠️ Lango heap critical: [sram]B SRAM free. Uptime: [X]." — otherwise NO message, NO notification, NO push. Stop after write_file.

- [daily 22:00] Nightly check. Call system_info. Read /lfs/memory/soak.md (last 10 lines via read_file). Send telegram_send_message: "🌙 Nightly: uptime [X]h, heap [X]KB (min [X]KB), psram [X]MB, rssi [X]dBm. [count soak lines today / any concern / all-clear]."

- [daily Mon 06:50] Weekly ARM + PC digest. web_search "ARM Holdings news this week". web_search "PC Chromebook market news this week". send_email subject "📊 Weekly ARM + PC Intel — [date]". Top 3 from each in one sentence each, under 300 words, no markdown.

- [daily Sat 18:00] Weekend surf check for Sunday at Pacifica Linda Mar (beginner surfer — James). noaa_buoy station 46012 (metres→feet: 1m≈3.3ft; Linda Mar ≈60% of open-ocean buoy). web_search "Pacifica Linda Mar surf forecast Sunday [date]". Beginner rating: ✅GO 1-3ft clean · ⚠️MAYBE 3-4ft · ❌SKIP 4ft+. telegram_send_message: "🏄 Sunday Linda Mar: [Xft @ Xs period, Xkt wind dir]. ⭐X/5 — [GO/MAYBE/SKIP]. [One sentence reason]." Be honest — James trusts the verdict.
