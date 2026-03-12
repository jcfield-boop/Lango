# Heartbeat Tasks

Runs every 30 minutes. Keep tasks fast and cheap — no web_search, no email.

- [x] Call get_current_time, then write a one-line status entry to today's daily note at /lfs/memory/<YYYY-MM-DD>.md (append=true): "HH:MM PST — Lango heartbeat OK. Heap: X KB."
- [x] Call system_info. If heap_free < 18000 (18 KB), send a Telegram alert: "⚠️ Lango heap low: X KB free. May need restart." (Normal idle range is 24–35 KB; only alert at true danger threshold.)
