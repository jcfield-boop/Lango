# Briefing Prefetch

Runs ~15 min before the morning briefing. Does ALL the live network
fan-out (weather, feeds, market searches, printer state) and writes the
results into ONE consolidated file `/lfs/memory/brief_data.md`. The
06:20 `brief001` then only `read_file`s that one file + composes the
email — turning a 6-9 iteration, 30-60 s briefing into a 2-iteration,
~15 s one, and spreading the flaky-wifi network calls out of the
user-visible critical path.

## When to use
Fired from cron `prefetch1` daily at **06:05 PDT** (15 min before
brief001 at 06:20). Also runnable manually ("refresh briefing data").

## Steps

Gather each item below. **A failed fetch is NOT fatal** — write
"unavailable" for that section and continue. The whole point is that
brief001 never has to touch the network, so partial data beats a
blocked briefing.

1. `read_file /lfs/memory/arm_stock_today.md` — ARM pre-market snapshot
   (already written by the 05:55 `armpre01` cron). Pass through verbatim.
2. `get_weather` for San Francisco — capture conditions, temp, today's range.
3. `web_search "ARM Holdings ARM stock price today"` — current price + % move.
4. `web_search "NASDAQ composite index today"` — index level + move.
5. `web_search "GBP USD exchange rate today"` — rate (or "unavailable" if no clear number — do NOT guess).
6. `rss_fetch https://hnrss.org/frontpage max_items=5` — top HN items.
7. `rss_fetch https://newsroom.arm.com/rss max_items=3` — ARM newsroom.
8. `klipper_request {"method":"GET","endpoint":"/printer/objects/query?print_stats"}` — printer state. On any error write "Moonraker unreachable". Never put a raw HTTP code in the file.

Then `write_file` (overwrite, NOT append) to `/lfs/memory/brief_data.md`
in EXACTLY this template so brief001 can parse it deterministically:

```
# Briefing data — prefetched <YYYY-MM-DD HH:MM PDT>

## ARM premarket
<contents of arm_stock_today.md, or "unavailable">

## Weather SF
<one line: conditions, temp, today range>

## Markets
ARM: <price, % move, or "unavailable">
NASDAQ: <level, move, or "unavailable">
GBP/USD: <rate, or "unavailable">

## HN top
- <item 1 title — 1-line why-it-matters to ARM/PC if relevant>
- <item 2 ...>
(up to 5)

## ARM newsroom
- <item 1>
- <item 2>
(up to 3)

## Printer
<state line: "idle" / "printing <file> NN%" / "Moonraker unreachable">
```

## Guardrails
- Max 9 tool calls (1 read + get_weather + 3 web_search + 2 rss_fetch +
  1 klipper + 1 write_file). Within LANG_AGENT_MAX_TOOL_ITER=10.
- **Silent on success — no Telegram, no email.** This is a background
  data-gathering task; brief001 is what notifies James.
- If `write_file` fails, retry ONCE; if it still fails, send ONE
  Telegram line to chat 5538967144: "⚠️ briefing prefetch write failed"
  so the morning brief degradation is at least visible.
- Stale-data note: stamp the prefetch time in the header. brief001
  checks it; if older than ~3 h (prefetch cron didn't run), brief001
  falls back to doing its own live calls per its own skill.

## Manual invocation
"refresh briefing data" / "prefetch the brief" → run the steps,
then reply via the originating channel with a one-line confirmation
("✓ briefing data refreshed — N HN items, markets OK, printer idle").
