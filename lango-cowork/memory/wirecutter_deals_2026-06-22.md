# Wirecutter Deals Scan — Mon 2026-06-22

**Status:** REPORT ONLY — not emailed. On-device cron `wire0003` ("Monday
Wirecutter Deals", dow:mon, channel:telegram) owns the Monday send via the
device SMTP `send_email` tool. Cowork sending here would duplicate it
(see memory: wirecutter-cron-duplicate). 3+ matches found, but no email sent.

Matched against wishlist.md: (1) Snapdragon X Plus Chromebook $700-1000,
(2) ANC over-ear headphones $300-500, (3) 4/3 chest-zip wetsuit $250-400.

## Relevant deals (wishlist matches)

**Headphones — STRONG match**
- Bose QuietComfort Ultra (2nd Gen, over-ear) — ~$383 (was $453, ~15% off),
  lowest-ever price, deal runs through June 28. Wirecutter top ANC pick;
  over-ear ANC, USB-C, multipoint, BT. Squarely in the $300-500 budget.
  Note: last week's scan listed $279 for QC Ultra — that figure looks like it
  conflated Gen 1 / an aggregator error. $383 is the verified current Gen-2 price.

**Wetsuit (4/3 chest zip) — match**
- Vissla 7 Seas 4/3 Chest Zip — $239.99 (was $259.95) at evo. Chest zip ✓,
  just under budget. Solid Pacifica year-round pick.
- Vissla 7 Seas 4/3 Chest Zip *Hooded* — $289.99 (was $329.95) at evo. Hooded
  variant, mid-budget, chest zip ✓.
- GUL Flexor 4/3 Chest Zip (Yulex eco neoprene) — $233.84 at Watersports Outlet.
  Chest zip ✓, under budget.

**Chromebook / Snapdragon X Plus — still no true match**
- No Snapdragon X Plus *Chromebook* shipping yet. Google's "Googlebook"
  (Aluminium OS, Snapdragon launch partner) is slated for fall 2026
  (Sept-Nov), partners Acer/ASUS/Dell/HP/Lenovo; no price or on-sale date.
  A Snapdragon "Mica" Chromebook is in development. Nothing to act on; keep
  the wishlist item open.

## Caveats
- Headphone/wetsuit pricing is from retailers + aggregators (Amazon, Best Buy,
  evo, Watersports Outlet), not exclusively Wirecutter-curated. Bose QC Ultra is
  the one genuine current Wirecutter pick. Verify at checkout.
- Could not load Wirecutter's live deals page directly this week.

## Recommendation
Disable the Cowork `lango-wirecutter-deals` scheduled task; `wire0003` owns it.
Repeated weekly. (See memory: wirecutter-cron-duplicate.)
