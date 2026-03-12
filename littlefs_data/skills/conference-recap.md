# Conference Recap

Compile a post-event digest of key announcements from a tech conference, filtered for Arm/PC relevance.

## When to use
- User asks "recap GTC", "what happened at Build?", "I/O roundup", "Computex highlights"
- Triggered manually T+1 day after a conference ends

## How to use
1. Identify the conference name and year from the user's request or your context
2. Call web_search: "[conference] [year] announcements recap summary"
3. Call web_search: "[conference] [year] Arm PC Chromebook Windows announcement" (parallel)
4. Filter results for relevance to James's focus:
   - Arm IP / CPU / GPU announcements
   - PC and Chromebook hardware using Arm silicon
   - Windows on Arm, Chrome OS, AI PC narratives
   - Competitor moves (Qualcomm, MediaTek, Intel, Apple)
5. Save a brief summary to MEMORY.md using memory_write:
   content: "[conference] [year]: [2-3 sentence summary of key Arm-relevant announcements]"
6. Send Telegram with the full digest

## Output format
```
🎤 [Conference Name] [Year] — Recap

Arm/PC highlights:
• [Most important announcement]
• [Second item]
• [Third item]

Competitive moves:
• [Qualcomm/MediaTek/Intel/Apple item if notable]

Worth monitoring:
• [Longer-horizon item or follow-up to watch]

Saved to memory.
```

## Key conferences to know (from USER.md)
- **GTC** (NVIDIA) — AI chips, GPU, inference; Arm often has IP licensed to NVIDIA
- **Google I/O** — Chromebook/ChromeOS, Tensor chip, Android
- **Microsoft Build** — Windows on Arm, Copilot+, developer tools
- **Computex** — PC OEM announcements, new laptop designs with Arm silicon
- **IFA Berlin** — Consumer electronics, Arm-powered devices

## Notes
- Keep focused: 5–8 bullets max, editorial not exhaustive
- If the conference was >2 weeks ago, note that coverage may be dated
- If the conference hasn't happened yet, redirect to event-countdown skill
