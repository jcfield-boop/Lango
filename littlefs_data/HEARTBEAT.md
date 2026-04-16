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

🏄 Surf (weekend mornings only — BEGINNER-FOCUSED)
- [Wave Xft @ Xs period, wind direction/speed]
- [Beginner verdict: ✅ GO (1–3ft clean) / ⚠️ MAYBE (3–4ft, mention caveat) / ❌ SKIP (4ft+ too big, or >15kt onshore wind)]

🤖 Lango status
- Uptime [X]h, heap [X]KB (overnight min [X]KB), PSRAM [X]MB free, WiFi [X]dBm.
```

Keep total under 400 words. Use James's first name once. No markdown headers in the email body itself — use the emoji-prefix section labels shown above. Do NOT include generic world news; James reads London Times and NY Times himself. The briefing must feel like it was written by someone who knows James's actual job and interests.

- [daily 22:00] Nightly soak summary. Call system_info. Read /lfs/memory/soak.md. Use `telegram_send_message` (NOT say): "🌙 Nightly check: uptime [X]h, heap [X]KB (day min: [X]KB), [X] heartbeats logged today. [Any concerns or all-clear]." (No need to truncate soak.md — firmware caps it at 96 lines automatically on every append.)

- [daily 06:03] ARM pre-market data fetch. Call web_search with query "ARM Holdings ARMH stock price pre-market today". Extract: current/pre-market price, change % vs previous close, and any significant news headline from the last 24 hours. Write result to /lfs/memory/arm_stock_today.md (overwrite). No email or Telegram — this is a data feed consumed by the 06:05 morning briefing. Keep the file under 200 words.

- [daily Sat 18:00] Weekend surf forecast for James — **BEGINNER-FOCUSED**. James is a beginner surfer (see USER.md): comfortable in 1–3 ft, 4 ft is the upper limit, 5 ft+ is unsafe. The rating must reflect beginner-friendliness, NOT wave quality for advanced surfers. Steps:
  1. Call `noaa_buoy` with station 46012 (Half Moon Bay — nearest buoy to Pacifica/Linda Mar). Note the reported wave height is in METRES (1 m ≈ 3.3 ft); also note Linda Mar is in the lee of Pillar Point, so actual beach wave height is often 40–60% of the open-ocean buoy reading.
  2. Call `web_search` with query "Pacifica Linda Mar surf forecast Sunday [current date]" for a Surfline/Magicseaweed cross-check on the **beach**, not the buoy.
  3. Beginner rating scale (use this, NOT generic quality stars):
     - ⭐⭐⭐⭐⭐ = 1–3 ft clean, light/offshore wind → **perfect for James, GO**
     - ⭐⭐⭐⭐ = 2–3 ft with short chop or 3 ft+ with glassy conditions → good, GO
     - ⭐⭐⭐ = 3–4 ft, any wind → workable but pushing his limit, MAYBE
     - ⭐⭐ = 4–5 ft or 15 kt+ onshore → too big/messy, SKIP
     - ⭐ = 5 ft+ or stormy → unsafe for beginner, STAY OUT
  4. Use `telegram_send_message` (NOT say): "🏄 Sunday Linda Mar beginner check: [wave Xft @ Xs period, wind Xkt dir]. Rating ⭐[X]/5 — [GO / MAYBE / SKIP]. [One sentence reason — e.g. 'Clean 2ft peelers, light offshore — dawn patrol' or 'Buoy showing 6ft long-period swell, too powerful for beginners, stay in']."
  5. If the forecast is SKIP (4 ft+), say so plainly and suggest the alternative (sleep in, yoga, etc.) rather than sugar-coating it. James trusts the verdict to be honest.

- [daily Mon 06:20] Weekly ARM + PC/Chromebook intel digest. Call web_search "ARM Holdings news this week". Call web_search "PC Chromebook market news this week". Summarise top 3 items from each in one sentence each. Send EMAIL with subject "📊 Weekly ARM + PC Intel — [date]" to James. Body under 300 words, no markdown. Useful for James's role as PMM at Arm covering PC and Chromebook segments.
