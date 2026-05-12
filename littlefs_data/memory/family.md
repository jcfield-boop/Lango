# Family

Names, relationships, birthdays, and any context Lango should know.
The `bday0001` cron (when activated) fires 7 days before each birthday
with a gift-prompt nudge via Telegram.

## People

### Nikki — sister

- **Relationship:** James's sister
- **Location:** France
- **Languages:** Speaks French; James visits her in June 2026 (Jun 14–21)
- **Birthday:** TBD — fill in when James provides
- **Gift notes:** TBD

### Add more people below

Format per person:
```
### <Name> — <relationship>
- **Birthday:** YYYY-MM-DD
- **Notes:** anything Lango should mention proactively (allergies, hobbies, recent gifts)
```

## How birthdays surface

Once a birthday is filled in:
- 14 days before: mentioned briefly in Sunday "week ahead" preview (cron `wkahd001` — Tier 2)
- 7 days before: dedicated Telegram reminder via `bday0001` cron with gift suggestions
- 1 day before: morning briefing includes a "tomorrow is X's birthday" callout
- Day of: morning briefing leads with the birthday reminder

Lango only auto-fires reminders for people listed here. To stop reminders
for someone, delete their entry.
