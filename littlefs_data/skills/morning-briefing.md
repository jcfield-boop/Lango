# Morning Briefing

Personalized 06:20 PDT weekday briefing for James. Fires from cron (brief001).

## Steps

**Fast path — read prefetched data, do NOT make live network calls.**
The `prefetch1` cron at 06:05 already did all the weather/markets/feeds/
printer fan-out and wrote `/lfs/memory/brief_data.md`. This skill's job
is to READ + COMPOSE, nothing more. Target ≤2 ReAct iterations.

1. `read_file /lfs/memory/brief_data.md` — the consolidated prefetched
   data (ARM premarket, Weather SF, Markets, HN top, ARM newsroom,
   Printer). **Check the timestamp in its header.** If it's missing or
   older than ~3 h, the prefetch cron didn't run — fall back to the
   "slow path" at the bottom of this skill.
2. `read_file /lfs/config/USER.md` — James's profile (focus areas, ongoing projects).
3. `read_file /lfs/memory/MEMORY.md` — pending items, TODOs, anything to remember.
4. `read_file /lfs/memory/calendar.md` — next-14-days appointments (📅 Today).
5. `read_file /lfs/memory/reading.md` — reading queue (📚 first item).
6. `read_file /lfs/memory/family.md` — birthdays within 7 days → 📅 Today.

   (Steps 1-6 are all `read_file` and can be issued as parallel tool
   calls in a single iteration — they don't depend on each other.)

7. Compose the email from the files above. No web_search, no rss_fetch,
   no get_weather, no klipper_request — that data is already in
   brief_data.md. **On Fri/Sat/Sun only**, the prefetch does NOT
   include surf, so make ONE `noaa_buoy` station 46012 call for the
   🏄 Surf section (buoy metres; Linda Mar ≈ 60% of buoy; beginner
   limit 3 ft). Mon-Thu skip Surf entirely.
8. `send_email` with:
   - to: `jcfield@gmail.com`
   - subject: `☀️ Lango Morning Briefing — [weekday, date]`
   - body: **target 600-900 words** of substantive content, sections in this order:
     - 📈 **ARM & Markets** — from brief_data.md "## ARM premarket" + "## Markets". ARMH price + day's % move + NASDAQ + GBP/USD context. 80-120 words. If a value is "unavailable" say so plainly, don't guess.
     - 💻 **PC/Chromebook Intel** — 2-3 items drawn from brief_data.md "## HN top" + "## ARM newsroom", filtered to ARM/PC/Chromebook relevance. For each: 1-line headline + 1-2 sentences why it matters to ARM PMM.
     - 📅 **Today** — anything from `/lfs/memory/calendar.md` within the next 24h. If nothing today but something tomorrow worth flagging, mention it. Also surface a birthday from `/lfs/memory/family.md` if within 7 days.
     - 📚 **Today's Read** — the FIRST item under `## Queue` in `/lfs/memory/reading.md`, formatted as: one-line headline + 1 sentence why-it-matters. If the file is missing or has no `## Queue` items, skip the section entirely (do not write "no items").
     - 📅 **This Week** — pull from MEMORY.md and USER.md. List pending TODOs, upcoming events/meetings 2-14 days out, anything James asked to remember, any focus areas from his profile that warrant a status callout. If genuinely nothing is queued, say so in one line — but check both files thoroughly first.
     - 🖨️ **Printer** — copy the "## Printer" line from brief_data.md verbatim (it's already a clean one-liner: "idle" / "printing <file> NN%" / "Moonraker unreachable"). Do NOT call klipper yourself.
     - 🏄 **Surf** (Fri/Sat/Sun only) — verdict GO/MAYBE/SKIP plus 1 sentence reasoning.
     - 🤖 **Lango status** — one line: uptime + SRAM free.

## Guardrails

- **Substantive ≠ verbose.** 600-900 words means each section has room to breathe (a paragraph each, not a single sentence). Don't pad with filler — but don't truncate mid-thought either. If you have less to say, say it well.
- No generic world news — stay ARM/PC/Chromebook focused.
- Use James's first name once in the greeting.
- **Fast path tool budget: 6 read_file (parallelisable) + 1 send_email + maybe 1 noaa_buoy = ~3 ReAct iterations.** No live web/rss/weather/klipper — that's all in brief_data.md. This is the whole point: keep the briefing turn short and reliable.
- If `send_email` fails (returns non-OK), fall back to `telegram_send_message` to chat 5538967144 with the FULL email body verbatim (the bot auto-splits at 4096 chars). Do NOT condense — James prefers a long Telegram message over a missing email.

## Slow path (fallback only — prefetch didn't run)

If `/lfs/memory/brief_data.md` is missing or its header timestamp is
older than ~3 h, the prefetch cron failed. In that case, do the live
gathering inline as a fallback: `get_weather`, `web_search` for
ARM/NASDAQ/GBP (parallel), `rss_fetch` hnrss + arm newsroom (parallel),
`klipper_request /printer/objects/query?print_stats` (never surface a
raw HTTP code — "Moonraker unreachable" on error), then compose + send
exactly as above. This is the old behaviour; it's slower and may run
6-9 iterations, but it guarantees a briefing even if prefetch broke.
