# Morning Briefing

Personalized 06:20 PDT weekday briefing for James. Fires from cron (brief001).

## Steps

1. `read_file /lfs/memory/arm_stock_today.md` — ARM pre-market snapshot from the 05:55 armpre01 cron.
2. `read_file /lfs/config/USER.md` — James's profile (name, timezone, role context).
3. `read_file /lfs/memory/MEMORY.md` — recent context / pending items.
4. `web_search "Arm Holdings Qualcomm Snapdragon PC Chromebook news today"` — pick 2-3 items relevant to James's Arm PMM role covering PC/Chromebook segment.
5. `web_search "NASDAQ today"` — market context.
6. **Only on Fri/Sat/Sun:** `noaa_buoy` station 46012 for Pacifica surf (buoy in metres; Linda Mar ≈ 60% of buoy reading; beginner limit 3 ft).
7. `send_email` with:
   - to: `jcfield@gmail.com`
   - subject: `☀️ Lango Morning Briefing — [weekday, date]`
   - body: **target 600-900 words** of substantive content, sections in this order:
     - 📈 **ARM & Markets** — ARMH price + day's % move + NASDAQ context. 80-120 words. Mention any notable analyst notes / earnings calendar items if web_search surfaces them.
     - 💻 **PC/Chromebook Intel** — 2-3 curated items. For each: 1-line headline + 1-2 sentences explaining why it matters to ARM PMM. Don't just dump the headline.
     - 📅 **This Week** — pull from MEMORY.md if present. If empty, include 1-line "no calendar items in memory."
     - 🏄 **Surf** (Fri/Sat/Sun only) — verdict GO/MAYBE/SKIP plus 1 sentence reasoning.
     - 🤖 **Lango status** — one line: uptime + SRAM free.

## Guardrails

- **Substantive ≠ verbose.** 600-900 words means each section has room to breathe (a paragraph each, not a single sentence). Don't pad with filler — but don't truncate mid-thought either. If you have less to say, say it well.
- No generic world news — stay ARM/PC/Chromebook focused.
- Use James's first name once in the greeting.
- If `arm_stock_today.md` is missing or stale (>24h), still send the briefing but note "(pre-market data unavailable)" in the ARM section.
- Max 8 tool calls total (within LANG_AGENT_MAX_TOOL_ITER=10 budget).
- If `send_email` fails (returns non-OK), fall back to `telegram_send_message` to chat 5538967144 with the FULL email body verbatim (the bot auto-splits at 4096 chars). Do NOT condense — James prefers a long Telegram message over a missing email.
