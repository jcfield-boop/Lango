# Ambient Cache — answer common queries from prefetched data

The `prefetch` cron refreshes `/lfs/memory/brief_data.md` every 4 hours
with weather, ARM/markets, top news, and printer state. For the most
common ad-hoc questions, **read that file first** instead of making a
live network call. This makes answers near-instant and — crucially —
keeps working when outbound wifi is flaky (the recurring failure mode:
DNS/socket wedges that make live weather/search/klipper calls hang
10-40 s then fail).

## When to use this (check brief_data.md FIRST)

If the user asks any of these on voice or Telegram, this skill applies:

- **Weather** — "what's the weather", "is it raining", "forecast" →
  use the `## Weather in San Francisco` section.
- **Markets** — "how's ARM", "ARM stock", "NASDAQ", "the market",
  "pound to dollar", "GBP USD" → use `## ARM Stock` / `## NASDAQ` /
  `## GBP/USD` sections.
- **News** — "what's the news", "anything happening", "tech news",
  "ARM news" → use `## Recent HN` / `## Recent ARM News`.
- **Printer** — "is the printer running", "print status", "how's the
  print", "is it done" → use `## Printer Status`.

## How

1. `read_file /lfs/memory/brief_data.md`.
2. Check the timestamp in the header (`# Briefing Data - <date/time>`).
3. **If the file exists and is < 4 h old:** answer from it directly.
   Always prefix with the freshness, e.g. "As of 14:05 — SF is 12°C
   and overcast" / "ARM was $228.50 (+0.9%) as of the 10:05 check."
   This sets the right expectation: it's recent, not real-time.
4. **If the file is missing, malformed, or > 4 h old, OR the user
   explicitly asks for "right now / live / latest / current price":**
   fall through to the normal live tool (`get_weather`, `web_search`,
   `klipper_request`) as you would without this skill.

## Why "as of HH:MM" matters

The user must always know whether a number is live or cached. A cached
ARM price quoted as if live is the kind of error we've fought before.
"As of the 10:05 check" is honest and still useful — for "what's the
weather" nobody needs sub-hour freshness, and a 1-second cached answer
that always works beats a 30-second live call that fails half the time
on this network.

## Out of scope

- Anything time-critical or transactional (reminders, sending
  messages, device control, "what time is it") — those are NOT in
  brief_data.md; handle them normally.
- If the user asks a follow-up that needs detail beyond the cached
  one-liner ("what's the hourly forecast"), do the live call —
  the cache is a fast first answer, not a replacement for depth.
