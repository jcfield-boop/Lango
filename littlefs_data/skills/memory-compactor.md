# Memory Compactor

Two modes depending on how it is triggered:

**Mode A — Fact save** (interactive / user-triggered): persist new facts from the current conversation.
**Mode B — Compact** (weekly cron `cmpct006`): deduplicate and trim MEMORY.md with no conversation context.

## When to use
- **Mode A**: user says "remember this", "save that", "keep in mind", "note that", or after a turn where new facts were learned
- **Mode B**: triggered by cron with message "Run the memory-compactor skill" — no live conversation context

## How to use

### Mode A — Save new facts
1. Read `/lfs/memory/MEMORY.md` — understand what is already stored
2. Identify NEW facts from the current conversation not yet in MEMORY.md:
   - User preferences, schedule changes, hobbies, contacts
   - Events, travel, reminders, deadlines
   - Tech stack, services, automations
   - Lessons learned from failures or corrections
3. Append only NEW facts using write_file (append=true) to `/lfs/memory/MEMORY.md`
   Format: `- [YYYY-MM-DD] <fact>`

### Mode B — Compact (cron)
1. Read `/lfs/memory/MEMORY.md` in full
2. Remove entries that are: exact duplicates, superseded by a newer entry, or clearly stale (>6 months old and no longer relevant)
3. Merge related facts into single concise entries where appropriate
4. Rewrite the whole file under 6 KB using write_file (overwrite)
5. Log a one-line summary: "Compacted: removed N entries, merged M, final size X KB"

### Both modes: size check
If MEMORY.md > 5 KB after writing, consolidate using Mode B steps.
Check system_info: if heap_free < 18 KB, send a low-heap email alert:
  Subject: "Lango low-heap warning" — Body: "Heap: X KB free. Recommend rebooting."

## What NOT to store
- One-time queries ("what's the weather")
- Information already captured in USER.md or SOUL.md
- Temporary task state or work-in-progress

## Output
Confirm: "Saved N new facts to memory." or "No new facts to save."
