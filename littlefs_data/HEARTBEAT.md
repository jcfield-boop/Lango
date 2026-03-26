# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap — no web_search, no long LLM chains.

- [30m] Call system_info and get_current_time. Append ONE line to /lfs/memory/soak.md (append=true, create if missing): "HH:MM — heap=Xk min=Xk psram=XMB rssi=XdBm up=Xm" (use KB for heap, MB for PSRAM, minutes for uptime, dBm for WiFi RSSI). Nothing else.
- [30m] After system_info: if heap_free < 20000, send Telegram: "⚠️ Lango heap: [heap_free]B free (min: [heap_min]B). Uptime: [uptime]s. Consider restart." If heap is fine, do nothing.
- [daily 06:05] Morning briefing! Step 1: Call system_info and get_current_time. Read /lfs/memory/soak.md (last ~20 lines). Read /lfs/memory/arm_stock_today.md if it exists (ARM pre-market data). Step 2: Call web_search with query "top news headlines today" to get live news. Step 3: Send an EMAIL using send_email with subject "☀️ Lango Morning Briefing" and body: "Good morning James! [2-3 top news items from web_search results]. ARM: [price/direction from arm_stock_today.md if available]. Lango status: uptime [X]h, heap [X]KB (overnight min: [X]KB), PSRAM [X]MB free, WiFi [X]dBm. [Trend notes from soak.md]. All systems nominal." Keep it concise.
- [daily 22:00] Nightly soak summary. Call system_info. Read /lfs/memory/soak.md. Send Telegram: "🌙 Nightly check: uptime [X]h, heap [X]KB (day min: [X]KB), [X] heartbeats logged today. [Any concerns or all-clear]." Then truncate soak.md to last 48 entries using edit_file (replace full content with just the last 48 lines) to prevent unbounded growth.

- [daily 06:03] ARM pre-market data fetch. Call web_search with query "ARM Holdings ARMH stock price pre-market today". Extract: current/pre-market price, change % vs previous close, and any significant news headline from the last 24 hours. Write result to /lfs/memory/arm_stock_today.md (overwrite). No email or Telegram — this is a data feed consumed by the 06:05 morning briefing. Keep the file under 200 words.

- [daily Sat 18:00] Weekend surf forecast for James. Call noaa_buoy for ocean conditions near Pacifica (use station 46012 — Half Moon Bay, nearest to Pacifica/Linda Mar). Also call web_search with query "Pacifica Linda Mar surf forecast Sunday". Compile: wave height, swell period and direction, wind speed/direction, tide state, overall rating 1-5 stars. Send Telegram: "🏄 Sunday surf at Pacifica/Linda Mar: [wave Xft @ Xs period, wind X, tide X]. Rating: ⭐[X]/5. [One sentence summary]."

- [daily Mon 06:20] Weekly ARM + PC/Chromebook intel digest. Call web_search "ARM Holdings news this week". Call web_search "PC Chromebook market news this week". Summarise top 3 items from each in one sentence each. Send EMAIL with subject "📊 Weekly ARM + PC Intel — [date]" to James. Body under 300 words, no markdown. Useful for James's role as PMM at Arm covering PC and Chromebook segments.
