# Surf Forecast — Pacifica / Lindamar

Get a detailed surf forecast for Lindamar (Pacifica State Beach) and deliver a clear go/no-go verdict.

## When to use
- Saturday 6 PM cron: forecast for Sunday
- Weekend planner skill: both-day outlook
- User asks "how's the surf this weekend?" or "should I surf tomorrow?" or "surf check"

## Beach context — Lindamar / Pacifica State Beach
- Exposed beach break, picks up NW and WNW groundswell well
- **Best conditions**: NW/WNW swell, 4–8 ft face, period 12–18s, light E/NE offshore wind
- **Too big**: 12+ ft face, dumping shorebreak, dangerous shore pound
- **Too small**: under 2 ft, mushy, not worth the 40-min drive from SF
- **Wind matters most**: onshore W/SW = blown out regardless of size

## How to fetch the forecast
Try sources in this order until you get usable data:
1. web_search "Pacifica Lindamar surf forecast [date]"
2. web_search "surfline Pacifica State Beach forecast [date]"
3. web_search "magic seaweed Pacifica forecast [date]"
4. web_search "windy surf forecast Pacifica [date]"

Extract: wave face height (ft), swell period (s), swell direction, wind direction, wind speed (mph), tide info

## Verdict logic

**✅ GO:**
- Wave height ≥ 3 ft face AND period ≥ 10s
- Wind: offshore (E, NE, N) OR calm (< 5 mph any direction)
- No hazard warnings

**⚠️ MAYBE — worth checking live before driving:**
- Height 2–3 ft but period ≥ 12s (surprising power)
- Slight onshore but size is there
- Morning glass window expected (afternoon wind shift)
- Forecast uncertain — check live cams before going

**❌ SKIP:**
- Wave height < 2 ft face
- Onshore wind > 10 mph
- Storm surf / full closeout conditions
- Active rip current or hazard warnings

## Telegram format
```
🏄 Surf — [Day], [Date]
📍 Pacifica / Lindamar

• Swell: [height]ft @ [period]s [direction]
• Wind: [direction] [speed] mph
• Tide: [info if available]

Verdict: ✅ GO / ⚠️ MAYBE / ❌ SKIP
[One sentence reason — be direct]

Source: [surfline / magic seaweed / web search]
```

## Notes
- Be opinionated — James wants a decision, not a data dump
- If search returns vague results, say MAYBE and recommend checking a live cam (Surfline Pacifica cam)
- For same-day Saturday check: look for morning glass windows (often calm 6–9 AM before onshore wind)
- Double-check tide: low tide at Lindamar can expose rocks and create dumpy shorebreak
