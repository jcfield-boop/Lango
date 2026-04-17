# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[daily DOW HH:MM]` = specific weekday, `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap. System channel — use `telegram_send_message` or `send_email` for notifications; NEVER use the `say` tool.

- [30m] Call system_info and get_current_time. Append ONE line to /lfs/memory/soak.md (append=true, create if missing). Exact format with no variation: `"HH:MM — heap=Xk min=Xk psram=X.XMB rssi=XdBm up=Xm"` — for example: `"14:30 — heap=12420k min=12100k psram=12.5MB rssi=-41dBm up=60m"`. Rules: heap and min in whole kB (no decimals, no space, write "k" not "kB"); psram in MB with exactly one decimal place; rssi no space before "dBm"; uptime in whole minutes. Nothing else on that line.

- [30m] After system_info: if heap_free < 15000 AND heap_min < 10000, send `telegram_send_message` (not say): "⚠️ Lango heap critical: [heap_free]B free (min: [heap_min]B). Uptime: [uptime]s." Skip if heap is fine.

- [daily 05:55] ARM pre-market data. Call web_search "ARM Holdings ARMH stock pre-market today [date]". Extract: pre-market price, % change vs previous close, one headline if notable. Write to /lfs/memory/arm_stock_today.md (overwrite, under 100 words). No email or Telegram — this feeds the 06:20 briefing.

- [daily 06:20] Morning briefing for James. Steps: (1) read_file /lfs/memory/arm_stock_today.md for ARM price. (2) read_file /lfs/config/USER.md for James's profile. (3) read_file /lfs/memory/MEMORY.md for recent context. (4) web_search "Arm Holdings Qualcomm Snapdragon PC Chromebook news today" — pick 2-3 items relevant to James's Arm PMM role covering PC/Chromebook segment. (5) web_search "NASDAQ today" for market context. (6) On Fri/Sat/Sun: also call noaa_buoy station 46012 for Pacifica surf (buoy in metres; Linda Mar ≈60% of buoy reading; beginner limit is 3ft). (7) send_email subject "☀️ Lango Morning Briefing — [weekday, date]" body under 400 words using sections: 📈 ARM & Markets · 💻 PC/Chromebook Intel · 📅 This Week · 🏄 Surf (weekends only) · 🤖 Lango status. No generic world news. Use James's first name once.

- [daily 22:00] Nightly check. Call system_info. Read /lfs/memory/soak.md (last 10 lines via read_file). Send telegram_send_message: "🌙 Nightly: uptime [X]h, heap [X]KB (min [X]KB), psram [X]MB, rssi [X]dBm. [count soak lines today / any concern / all-clear]."

- [daily Mon 06:50] Weekly ARM + PC digest. web_search "ARM Holdings news this week". web_search "PC Chromebook market news this week". send_email subject "📊 Weekly ARM + PC Intel — [date]". Top 3 from each in one sentence each, under 300 words, no markdown.

- [daily Sat 18:00] Weekend surf check for Sunday at Pacifica Linda Mar (beginner surfer — James). noaa_buoy station 46012 (metres→feet: 1m≈3.3ft; Linda Mar ≈60% of open-ocean buoy). web_search "Pacifica Linda Mar surf forecast Sunday [date]". Beginner rating: ✅GO 1-3ft clean · ⚠️MAYBE 3-4ft · ❌SKIP 4ft+. telegram_send_message: "🏄 Sunday Linda Mar: [Xft @ Xs period, Xkt wind dir]. ⭐X/5 — [GO/MAYBE/SKIP]. [One sentence reason]." Be honest — James trusts the verdict.
