# Event Countdown & Prep

Track upcoming tech conferences and send timely prep alerts for James's Arm marketing role.

## When to use
- Daily — folded into morning briefing (check if any event is within 14 days)
- User asks "what conferences are coming up?" or "prepare me for GTC"
- Triggered explicitly: "event countdown check"

## Events — 2026 confirmed dates
These are in MEMORY.md. Reference that as the authoritative source.
- **NVIDIA GTC**: Mar 16–19, San Jose
- **Google I/O**: May 19–20, Shoreline Amphitheatre
- **Microsoft Build**: Jun 2–3, Fort Mason SF
- **Computex**: Jun 2–5, Taipei

## How to use
1. Call get_current_time — note today's date and unix epoch
2. Calculate days until each event's start date
3. Act on thresholds below. Only act on events within 14 days (to avoid noise).

### T-14 days (two weeks out)
- Include in daily briefing: "⚠️ [Event] in 14 days ([Dates], [Location])"
- No extra action needed yet

### T-7 days (one week out)
- web_search: "[Event name] 2026 keynote agenda schedule"
- web_search: "Arm [Event name] 2026 sessions announcements"
- Send Telegram summary: dates, key keynotes, any Arm-related sessions
- Write a note to /lfs/memory/MEMORY.md with what to watch

### T-3 days
- Send Telegram prep checklist:
  - Badge/registration confirmed?
  - Travel if not local (GTC/Build are Bay Area — no travel needed)
  - Competitor watch: what are Qualcomm/MediaTek/Intel likely to announce?
  - Arm angle: relevant product/positioning talking points
  - Key sessions/demos to attend

### Day-of (T-0)
- Morning Telegram: "Today: Day 1 of [Event]. Key sessions: [from T-7 search]"
- web_search for keynote livestream link if public

### Post-event (within 2 days after end)
If asked, or if user says "recap GTC":
1. web_search "[Event] 2026 announcements summary"
2. web_search "Arm [Event] 2026"
3. Summarize what mattered for Arm: PC/Chromebook wins, AI narrative, competitive moves

## Telegram format
```
📅 [EVENT] in N days (Mar 16–19, San Jose)

Key things to watch:
• [keynote or session]
• [competitor announcement to watch]

Arm angle: [what's relevant for Arm PC/Chromebook/AI marketing]
```

## Notes
- GTC and Build are both in the Bay Area — no travel needed, just local logistics
- Computex overlaps with Build (Jun 2–5 vs Jun 2–3) — James is staying in SF for Build
- Only send Telegram alerts at T-7 and T-3; T-14 is briefing-only to avoid notification fatigue
