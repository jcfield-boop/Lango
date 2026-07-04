# Brief Preview — 2026-07-04 (Cowork run, NOT sent)

_Composed 06:35 PDT by Cowork scheduled task. Email NOT sent — on-device `brief001` cron (next_run 09:18 PDT today) owns delivery per feedback-email-relay memory. Preview below is what the briefing should contain._

**Subject:** ☀️ Lango Morning Briefing — Saturday, July 4

⚠️ Heads-up first: your Neptune 3 Pro extruder has been sitting at 220°C with NO print running since at least 04:05 (re-verified 06:34 — still 220.9°C, heater at 43%, state standby, bed off). If that's not an intentional preheat, kill it: Mainsail at 192.168.0.50 or `curl -X POST "http://192.168.0.50/printer/gcode/script?script=M104%20S0"`.

🌤 Weather — San Francisco
Mostly sunny for the 4th. High 68°F / Low 54°F, WSW wind 7–13 mph with gusts to 18. Marine layer early, clearing by evening — good fireworks conditions.

📈 Markets
• ARM: $315.28 (July 3 close, -6.6% Friday on AI/semi profit-taking, valuation concerns, and Qualcomm trial risk; +$1.12 after-hours)
• NASDAQ: 25,832.67 (-0.80%, July 2 close)
• GBP/USD: 1.3344
• US markets closed today for Independence Day; reopen Monday.
(Note: brief_data cache says markets were closed Jul 3; the 06:08 ARM snapshot reports a Jul 3 session. Snapshot used as it's fresher — worth a glance Monday.)

🦾 Arm & Ecosystem
• Neural Dawn: first use of Arm Neural Technology with Unreal Engine MegaLights on mobile — a step-change demo for mobile gaming graphics.
• Oracle Cloud Infrastructure joins the Arm AGI CPU ecosystem as agentic AI workloads accelerate.
• Arm-based NVIDIA RTX Spark positioning as the PC for the agentic era — directly relevant to your Large Screen Compute story.

🌐 Tech Headlines
• Gemini Code Assist shuts down July 17 — consolidation in AI dev tooling; watch for competitor positioning against Google's retreat.
• Virginia bans sale of geolocation data — state-level privacy regulation is accelerating; relevant to wearables/XR data narratives.
• Spain orders Palantir blacklisted from public and private companies — notable precedent for sovereign-tech procurement politics in Europe.

📅 Today
• Nothing scheduled — calendar clear through the 14-day window (next fixed item: IFA Berlin, Sept).
• 🎆 July 4th: Golden Gate Bridge fireworks ~9:30 PM, launched from the bridge itself (third time in ~a century). Best viewing: Crissy Field or Marina Green. Quieter option: Redwood City drone show, 9:30 PM.

🧠 From Memory
• Printer extruder heater — see top. Been on for hours unattended.
• France trip wrapped (Jun 14–21) — nothing pending.

🏄 Surf — Linda Mar
• NW swell filling in this morning: ~5 ft @ 8s primary + 1.5 ft @ 14s ground swell underneath. Wind cross-offshore/light SSW early, building onshore later. (NDBC 46012 real-time feed unreachable this run; figures from Surfline/surf-forecast via web search + last night's outlook — consistent with each other.)
• Verdict: ❌ SKIP. 5 ft @ 8s is over your 4 ft line — short-period NW at that size means punchy, closed-out Linda Mar. If you must look, dawn is the only window before wind ruins it; Sunday may ease as the 8s energy fades. Check the 5 PM surf email for tomorrow's call.

---
## Run notes (Cowork)
- Fast path used: brief_data.md fresh (04:06 PDT, 2.4 h old); arm_stock_today.md fresh (06:08 PDT).
- USER.md + MEMORY.md read from device (192.168.0.44 /api/file). calendar.md / family.md / reading.md not exposed via REST ("Unknown file name") — calendar covered by weekend_planner.md (clear); family birthdays and reading queue omitted, unavailable this run.
- No noaa_buoy tool in Cowork; 46012 real-time text blocked by fetch provenance; surf sourced from web search, matches Fri planner outlook.
- brief001 confirmed enabled, last_run Jul 3 09:18, next_run Jul 4 09:18 PDT → duplicate layer, so no send from Cowork.
- Printer heater anomaly re-verified live at 06:34 — persisting. No gcode sent (report-only run).
