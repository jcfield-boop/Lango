# ARM Ecosystem News Monitor

Weekly scan for Arm and competitive ecosystem news relevant to James's Product Marketing role at Arm.

## When to use
- Weekly Sunday morning cron
- User asks "any Arm news this week?" or "what did Qualcomm announce?"
- Pre-event scan (T-7 for any major conference — see event-countdown skill)

## What to watch

### Primary (always check)
- **Arm Holdings** — product announcements, partnerships, licensing news, CSS/IP releases
- **Qualcomm Snapdragon** — PC/Chromebook chips (X Elite, X Plus updates), mobile
- **MediaTek** — Dimensity/Kompanio PC and Chromebook chip announcements

### Secondary (when credits allow)
- **Google** — Chromebook hardware, ChromeOS features, Tensor chip news
- **Microsoft** — Copilot+ PC, Windows on Arm compatibility, new OEM designs
- **Apple** — M-series (competitive context for Arm's PC narrative)
- **PC OEMs** — HP, Dell, Lenovo Arm laptop/Chromebook announcements

## How to use
1. Note the current date and month from your system context
2. Arm newsroom RSS first (free, no API credits):
   - rss_fetch https://newsroom.arm.com/rss max_items=5
3. Competitive queries (use web_search, be efficient — max 4 calls):
   - web_search "Qualcomm Snapdragon PC Chromebook announcement [current month] [current year]"
   - web_search "MediaTek Dimensity Chromebook [current month] [current year]"
   - web_search "Windows on Arm Copilot+ PC news [current week]"
   - web_search "Google Chromebook Arm announcement [current month] [current year]"
4. Filter ruthlessly — skip consumer mobile-only news, focus on PC/Chromebook/AI/infrastructure
5. Compile 5–8 bullets max, prioritized by relevance to Arm's PC/Chromebook business
6. Send via Telegram

## Telegram output format
```
🦾 Arm Ecosystem — Week of [Date]

Arm:
• [item from newsroom]

Competitive:
• [Qualcomm/MediaTek item]
• [Google/MSFT item]

Worth watching:
• [1–2 longer-horizon items or upcoming announcements]
```

## Cost discipline
- Always rss_fetch Arm newsroom before web_search
- Max 4 web_search calls per weekly run
- Skip secondary sources if primary sources already give 5+ bullets
- If nothing significant happened this week, say so briefly — don't pad

## Notes
- James's focus: Arm's PC/Chromebook revenue story, AI PC narrative
- Competitive framing matters: "Qualcomm announced X — Arm angle is Y"
- This is editorial, not a dump — filter for what actually matters to his job
