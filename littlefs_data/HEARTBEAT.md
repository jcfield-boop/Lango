# Heartbeat Tasks

Markers: `[30m]` = every cycle, `[daily HH:MM]` = once/day (Pacific time), `[ ]` = one-shot, `[x]` = done.
Keep tasks fast and cheap — no web_search, no long LLM chains.

- [30m] Call system_info and get_current_time. Append ONE line to /lfs/memory/soak.md (append=true, create if missing): "HH:MM — heap=Xk min=Xk psram=XMB up=Xm" (use KB for heap, MB for PSRAM, minutes for uptime). Nothing else.
- [30m] After system_info: if heap_free < 20000, send Telegram: "⚠️ Lango heap: [heap_free]B free (min: [heap_min]B). Uptime: [uptime]s. Consider restart." If heap is fine, do nothing.
- [daily 06:05] Morning briefing! Call system_info and get_current_time. Read /lfs/memory/soak.md (last ~20 lines). Send an EMAIL using send_email with subject "☀️ Lango Morning Briefing" and body: "Good morning James! Lango status: uptime [X]h, heap [X]KB (overnight min: [X]KB), PSRAM [X]MB free. [Trend notes from soak.md — e.g. 'heap stable overnight' or 'heap trending down from X to Y']. All systems nominal." Keep it concise.
- [daily 22:00] Nightly soak summary. Call system_info. Read /lfs/memory/soak.md. Send Telegram: "🌙 Nightly check: uptime [X]h, heap [X]KB (day min: [X]KB), [X] heartbeats logged today. [Any concerns or all-clear]." Then truncate soak.md to last 48 entries using edit_file (replace full content with just the last 48 lines) to prevent unbounded growth.
