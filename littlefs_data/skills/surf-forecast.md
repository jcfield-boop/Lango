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
Use both sources and combine:

**Step 1 — Real buoy data (always do this first, no API cost):**
- `noaa_buoy` tool, station omitted (defaults to 46012 = Pt. Reyes)
- This gives: wave height (ft), dominant period (s), swell direction, wind speed/direction, water temp
- Note: buoy is ~30mi offshore NW of Pacifica — nearshore conditions often smaller by 20-30%

**Step 2 — Forecast (for tomorrow/future days):**
- Buoy data is current conditions only (not a forecast)
- For tomorrow's prediction, do ONE web_search:
  `web_search "Pacifica surf forecast [date]"` or `web_search "surfline Pacifica State Beach [date]"`

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
