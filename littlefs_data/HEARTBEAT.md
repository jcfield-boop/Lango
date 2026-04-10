# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap — no web_search on the 30m cycle, no long LLM chains.

**IMPORTANT: NEVER call the `say` tool from heartbeat tasks. All notifications must use `telegram_send_message` or `send_email`. The `say` tool is for live voice interactions only.**

- [30m] Call system_info and get_current_time. Append ONE line to /lfs/memory/soak.md (append=true, create if missing): "HH:MM — heap=Xk min=Xk psram=XMB rssi=XdBm up=Xm" (use KB for heap, MB for PSRAM, minutes for uptime, dBm for WiFi RSSI). Nothing else.

- [30m] After system_info: if heap_free < 15000 AND heap_min < 10000, use `telegram_send_message` (NEVER `say`) with: "⚠️ Lango heap critical: [heap_free]B free (min: [heap_min]B). Uptime: [uptime]s. Consider restart." If heap is fine, do nothing. DO NOT speak via speaker. DO NOT send for non-critical heap levels.

- [daily 06:05] **James's Morning Briefing** — this is the highest-value touchpoint of the day. Read USER.md for James's priorities. Compile a personalized email covering his actual interests, NOT generic headlines.
  1. Call `read_file` on /lfs/config/USER.md to refresh James's profile (Arm PMM, PC/Chromebook focus, ARM stock, surf spot, tech events).
  2. Call `read_file` on /lfs/memory/arm_stock_today.md (pre-market data written at 06:03).
  3. Call `read_file` on /lfs/memory/MEMORY.md (recent facts).
  4. Call `read_file` on /lfs/memory/soak.md (last ~10 lines for device trend).
  5. Call `system_info` and `get_current_time` for device stats.
  6. Call `web_search` ONCE with query "Arm Holdings Qualcomm Snapdragon PC Chromebook news today" — this covers James's business focus. Extract 2-3 items most relevant to PC/Chromebook segment.
  7. Call `web_search` ONCE with query "NASDAQ today" to get market open context.
  8. Check USER.md tech events calendar — if any upcoming event is within 14 days, mention the countdown.
  9. On Fri/Sat/Sun mornings, also call `noaa_buoy` with station 46012 for Pacifica surf conditions.
  10. Send EMAIL via `send_email` with subject "☀️ Lango Morning Briefing — [weekday, date]" and body structured as:

```
Good morning James.

📈 ARM & Markets
- ARM: [price + direction from arm_stock_today.md]
- NASDAQ: [level/direction]
- [One sentence on GBP/USD or global context if notable]

💻 PC & Chromebook Intel (your segment)
- [Item 1 — one sentence, relevant to Arm PMM role]
- [Item 2 — one sentence]
- [Item 3 — one sentence, only if genuinely notable]

📅 This Week
- [Next tech event from USER.md with day countdown, if within 14 days]
- [France trip countdown if within 60 days]
- [Other relevant calendar item]

🏄 Surf (weekend mornings only)
- [Wave height, period, wind, rating 1-5 from noaa_buoy]

🤖 Lango status
- Uptime [X]h, heap [X]KB (overnight min [X]KB), PSRAM [X]MB free, WiFi [X]dBm.
```

Keep total under 400 words. Use James's first name once. No markdown headers in the email body itself — use the emoji-prefix section labels shown above. Do NOT include generic world news; James reads London Times and NY Times himself. The briefing must feel like it was written by someone who knows James's actual job and interests.

- [daily 22:00] Nightly soak summary. Call system_info. Read /lfs/memory/soak.md. Use `telegram_send_message` (NOT say): "🌙 Nightly check: uptime [X]h, heap [X]KB (day min: [X]KB), [X] heartbeats logged today. [Any concerns or all-clear]." Then truncate soak.md to last 48 entries using edit_file (replace full content with just the last 48 lines) to prevent unbounded growth.

- [daily 06:03] ARM pre-market data fetch. Call web_search with query "ARM Holdings ARMH stock price pre-market today". Extract: current/pre-market price, change % vs previous close, and any significant news headline from the last 24 hours. Write result to /lfs/memory/arm_stock_today.md (overwrite). No email or Telegram — this is a data feed consumed by the 06:05 morning briefing. Keep the file under 200 words.

- [daily Sat 18:00] Weekend surf forecast for James. Call noaa_buoy for ocean conditions near Pacifica (use station 46012 — Half Moon Bay, nearest to Pacifica/Linda Mar). Also call web_search with query "Pacifica Linda Mar surf forecast Sunday". Compile: wave height, swell period and direction, wind speed/direction, tide state, overall rating 1-5 stars. Use `telegram_send_message` (NOT say): "🏄 Sunday surf at Pacifica/Linda Mar: [wave Xft @ Xs period, wind X, tide X]. Rating: ⭐[X]/5. [One sentence summary]."

- [daily Mon 06:20] Weekly ARM + PC/Chromebook intel digest. Call web_search "ARM Holdings news this week". Call web_search "PC Chromebook market news this week". Summarise top 3 items from each in one sentence each. Send EMAIL with subject "📊 Weekly ARM + PC Intel — [date]" to James. Body under 300 words, no markdown. Useful for James's role as PMM at Arm covering PC and Chromebook segments.
