# Tech Events 2026

Look up exact dates for major tech conferences relevant to James's Arm marketing role.

## When to use
- User asks about conference dates, upcoming events, or tech calendar
- Morning briefing when an event is within 2 weeks
- User asks to "update the tech events calendar"

## Key events to track (2026)
- **NVIDIA GTC** — GPU/AI developer conference (usually March)
- **Microsoft Build** — Developer conference (usually May)
- **Google I/O** — Developer conference (usually May)
- **Computex Taipei** — PC/chip industry (usually late May or early June)
- **IFA Berlin** — Consumer electronics (usually early September)
- **PC OEM private events** — HP, Dell, Lenovo (user to specify)

## How to look up dates
1. Call get_current_time for current date
2. For each unconfirmed event, call web_search: "[event name] 2026 dates"
3. Extract confirmed dates from search results
4. Update USER.md: call write_file with append=false to update the Tech Events Calendar section
5. Also update MEMORY.md with any newly confirmed dates

## When to alert
In the morning briefing, if any event is within 14 days, add a reminder:
"⚠️ [Event] is in N days (Date–Date, Location)"

## Format for USER.md update
Under "## Tech Events Calendar" → "### 2026 Upcoming":
```
- **[Event]**: [Month Day–Day], [Location] (confirmed [lookup date])
```

## Notes
- Arm's PC & Chromebook focus makes Computex and Google I/O highest priority
- GTC matters for AI/GPU positioning narrative
- IFA is European consumer electronics — relevant for Arm's TV/STB/mobile
