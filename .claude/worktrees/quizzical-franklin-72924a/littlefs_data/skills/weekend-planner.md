# Weekend Planner

Every Friday at 5 PM PST, compile a concise "weekend at a glance" digest for James and send via Telegram.

## When to use
- Triggered by Friday 5 PM PST cron
- User asks "what's the plan for the weekend?" or "weekend outlook"

## How to use
1. Call get_current_time for date context (to know this Saturday/Sunday's dates)
2. Run lookups below, then compile and send via Telegram

### 1. Surf forecast (both days)
- web_search "Pacifica Lindamar surf forecast [Saturday date] [Sunday date]"
  OR web_search "surfline Pacifica State Beach forecast this weekend"
- Extract: wave height, period, wind direction/speed for each day
- Apply verdict: GO / MAYBE / SKIP (see surf-forecast skill for logic)
- Lindamar works best: NW swell, 4–8 ft face, 12s+ period, offshore/light wind

### 2. SF weather
- web_search "San Francisco weather forecast [Saturday date] [Sunday date]"
- Extract: high/low temps, rain chance, wind for each day

### 3. Week ahead preview
- Read /lfs/memory/MEMORY.md for upcoming events or dates
- Check if any tech event is within 14 days (GTC, I/O, Build, Computex)
- Note any cron-scheduled tasks firing next week

### 4. Travel countdown
- If France trip (June 14, 2026) is within 60 days, include countdown
- Include one actionable prep reminder if within 30 days (passport check, mail hold, etc.)

## Telegram output format
```
🌊 Weekend — Sat [date] & Sun [date]

🏄 Surf (Pacifica/Lindamar):
• Sat: [height]ft @ [period]s, [wind] — ✅ GO / ⚠️ MAYBE / ❌ SKIP
• Sun: [height]ft @ [period]s, [wind] — ✅ GO / ⚠️ MAYBE / ❌ SKIP

🌤️ SF Weather:
• Sat: [high]°F, [conditions]
• Sun: [high]°F, [conditions]

📅 Coming up:
• [Event or deadline if within 14 days]
• [Any other noteworthy item]

[🇫🇷 France in N days — only if within 60 days]
```

## Notes
- Be opinionated on surf — James wants a verdict, not just numbers
- Keep the whole thing under 20 lines — this is a glance, not a report
- If surf data is ambiguous, say MAYBE and note what to check live before driving
- Send Saturday morning surf-only update as a bonus if Saturday conditions look great
