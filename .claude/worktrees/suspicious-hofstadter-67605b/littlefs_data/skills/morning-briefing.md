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
   - body: under 400 words, sections in this order:
     - 📈 **ARM & Markets** (ARMH price, NASDAQ snapshot)
     - 💻 **PC/Chromebook Intel** (2-3 curated items)
     - 📅 **This Week** (pulled from MEMORY.md if present)
     - 🏄 **Surf** (Fri/Sat/Sun only — verdict GO/MAYBE/SKIP)
     - 🤖 **Lango status** (one line: uptime + SRAM free)

## Guardrails

- No generic world news — stay ARM/PC/Chromebook focused.
- Use James's first name once in the greeting.
- If `arm_stock_today.md` is missing or stale (>24h), still send the briefing but note "(pre-market data unavailable)" in the ARM section.
- Max 8 tool calls total (within LANG_AGENT_MAX_TOOL_ITER=10 budget).
- If `send_email` fails, fall back to `telegram_send_message` with a condensed version so James still gets the briefing.
