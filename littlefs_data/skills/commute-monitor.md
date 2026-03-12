# Commute Monitor

Check SF→San Jose traffic and send a Telegram alert before the morning commute.

## When to use
- Triggered daily at 6:00 AM PST by cron
- User asks "traffic check", "how's the commute?", "should I leave now?"

## Steps
1. Check the current day from your system context — **if it's Saturday or Sunday, skip and respond "Weekend — no commute check."**
2. Call web_search: "SF to San Jose I-280 traffic right now"
3. Also call web_search: "US-101 San Francisco to San Jose traffic incident" (parallel if possible)
4. Estimate drive time and identify any incidents on:
   - I-280 (preferred route from Valencia St, SF → Rose Orchard Way, San Jose)
   - US-101 (alternate)
5. Send Telegram with result

## Output format
```
🚗 Commute — [Day] [Time]
I-280: ~48 min (normal)
101: ~55 min (light traffic)
Depart ~6:30 AM as planned.
```

Or if incident detected:
```
🚗 Commute — [Day] [Time]
⚠️ I-280: ~72 min (accident near Hwy 92 interchange)
101: ~55 min — better option today
Consider departing earlier.
```

## Notes
- Route: 1438 Valencia St, SF → 120 Rose Orchard Way, San Jose (~42 miles)
- Preferred: I-280 South (scenic, usually faster before 7 AM)
- Alternate: US-101 South
- James typically departs ~6:30 AM
- Weekend check: return immediately without searching
