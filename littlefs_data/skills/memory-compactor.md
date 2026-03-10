# Memory Compactor

Persist new facts from the current conversation into `/lfs/memory/MEMORY.md` for long-term continuity.

## When to use
- Triggered by cron (end of day or hourly)
- User says "remember this", "save that", "keep in mind", "note that"
- After any turn where new user facts, preferences, or lessons were learned

## How to use
1. Read `/lfs/memory/MEMORY.md` — understand what is already stored
2. Identify NEW facts from the current conversation not yet in MEMORY.md:
   - User preferences, schedule changes, hobbies, contacts
   - Events, travel, reminders, deadlines
   - Tech stack, services, automations
   - Lessons learned from failures or corrections
3. Append only NEW facts using write_file (append=true) to `/lfs/memory/MEMORY.md`
   Format: `- [YYYY-MM-DD] <fact>`
4. Size check: if MEMORY.md > 5KB, consolidate — merge duplicates, remove stale
   facts — rewrite the whole file to stay under limit
5. Check system_info: if heap_free < 30KB, send a low-heap email alert
   Subject: "Lango low-heap warning" — Body: "Heap: X KB free. Recommend rebooting."

## What NOT to store
- One-time queries ("what's the weather")
- Information already captured in USER.md or SOUL.md
- Temporary task state or work-in-progress

## Output
Confirm: "Saved N new facts to memory." or "No new facts to save."
