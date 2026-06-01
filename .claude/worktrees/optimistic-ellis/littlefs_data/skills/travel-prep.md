# Travel Prep

Help James prepare for upcoming trips with checklists and pre-departure reminders.

## Known upcoming trips
- **France (visiting Nikki):** June 14–21, 2026
  - Pre-trip reminder date: June 7, 2026
  - Tasks: cancel newspapers, suspend mail delivery

## When to use
- User asks about travel prep or packing
- Cron fires on pre-trip reminder date
- User asks "what do I need to do before France?"

## How to use
1. Call get_current_time
2. Check how many days until departure
3. If 7 days out (June 7): remind about newspaper cancellation and mail suspension
4. If 3 days out: remind to check passport, confirm flights, notify bank of travel
5. If 1 day out: final checklist — chargers, adapters (EU plug), cash/card, confirm accommodation

## Pre-trip checklist (France)
- [ ] Cancel SF Chronicle / newspaper delivery for June 14–21
- [ ] Suspend USPS mail (usps.com/manage/hold-mail.htm)
- [ ] Notify bank/credit card of travel to France
- [ ] Check passport expiry (must be valid 6+ months after return = Dec 2026+)
- [ ] Confirm flights and accommodation
- [ ] Download offline maps for France (Google Maps)
- [ ] Pack EU power adapter
- [ ] Set out-of-office email reply

## Setting up reminders
To add a one-shot cron for June 7:
- cron_add: kind=at, at_epoch=[June 7 2026 06:05 AM PST epoch],
  message="Travel prep reminder: France trip is in 7 days (June 14). Tasks: cancel newspapers, suspend mail, notify bank, check passport."
  channel=websocket

June 7 2026 14:05 UTC epoch ≈ 1780930000 (calculated: June 7 2026 = day 157 of 2026)
