# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[daily DOW HH:MM]` = specific weekday, `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap. System channel — use `telegram_send_message` or `send_email` for notifications; NEVER use the `say` tool.

- [30m] Call system_info (use HH:MM from the heartbeat check time above). Then: (1) Write ONE line append=true to /lfs/memory/soak.md using write_file. Exact format: `"HH:MM — heap=Xk min=Xk psram=X.XMB rssi=XdBm up=Xm"` — example: `"14:30 — heap=12420k min=12100k psram=12.5MB rssi=-41dBm up=60m"`. Use "Free heap" for heap/min in whole kB; PSRAM for psram with one decimal; rssi no space before "dBm"; uptime in whole minutes. (2) Check "SRAM free" from system_info: ONLY if SRAM free < 15000 AND min heap < 10000, send one telegram_send_message "⚠️ Lango heap critical: [sram]B SRAM free. Uptime: [X]." — otherwise NO message, NO notification, NO push. Stop after write_file.

- [daily 05:55] ARM pre-market data. Call web_search "ARM Holdings ARMH stock pre-market today [date]". Extract: pre-market price, % change vs previous close, one headline if notable. Write to /lfs/memory/arm_stock_today.md (overwrite, under 100 words). No email or Telegram — this feeds the 06:20 briefing.

- [daily 06:20] Morning briefing for James. Steps: (1) read_file /lfs/memory/arm_stock_today.md for ARM price. (2) read_file /lfs/config/USER.md for James's profile. (3) read_file /lfs/memory/MEMORY.md for recent context. (4) web_search "Arm Holdings Qualcomm Snapdragon PC Chromebook news today" — pick 2-3 items relevant to James's Arm PMM role covering PC/Chromebook segment. (5) web_search "NASDAQ today" for market context. (6) On Fri/Sat/Sun: also call noaa_buoy station 46012 for Pacifica surf (buoy in metres; Linda Mar ≈60% of buoy reading; beginner limit is 3ft). (7) send_email subject "☀️ Lango Morning Briefing — [weekday, date]" body under 400 words using sections: 📈 ARM & Markets · 💻 PC/Chromebook Intel · 📅 This Week · 🏄 Surf (weekends only) · 🤖 Lango status. No generic world news. Use James's first name once.

- [daily 22:00] Nightly check. Call system_info. Read /lfs/memory/soak.md (last 10 lines via read_file). Send telegram_send_message: "🌙 Nightly: uptime [X]h, heap [X]KB (min [X]KB), psram [X]MB, rssi [X]dBm. [count soak lines today / any concern / all-clear]."

- [daily Mon 06:50] Weekly ARM + PC digest. web_search "ARM Holdings news this week". web_search "PC Chromebook market news this week". send_email subject "📊 Weekly ARM + PC Intel — [date]". Top 3 from each in one sentence each, under 300 words, no markdown.

- [daily Sat 18:00] Weekend surf check for Sunday at Pacifica Linda Mar (beginner surfer — James). noaa_buoy station 46012 (metres→feet: 1m≈3.3ft; Linda Mar ≈60% of open-ocean buoy). web_search "Pacifica Linda Mar surf forecast Sunday [date]". Beginner rating: ✅GO 1-3ft clean · ⚠️MAYBE 3-4ft · ❌SKIP 4ft+. telegram_send_message: "🏄 Sunday Linda Mar: [Xft @ Xs period, Xkt wind dir]. ⭐X/5 — [GO/MAYBE/SKIP]. [One sentence reason]." Be honest — James trusts the verdict.
