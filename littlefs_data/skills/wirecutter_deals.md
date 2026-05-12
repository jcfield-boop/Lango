# Wirecutter Deals Skill — personalised

Personalised version of the Monday deals digest. Instead of dumping a
generic "consumer electronics" list, this matches deals against James's
own wishlist (`/lfs/memory/wishlist.md`) and only surfaces relevant ones.

## Schedule

- **Frequency:** Weekly (every Monday at 10:00 AM PST), cron id `wire0003`
- **Delivery:** Telegram (chat 5538967144). Email as fallback if Telegram fails.

## Steps

1. `read_file /lfs/memory/wishlist.md` — James's curated shopping intent.
   Parse the `## Items` section; each line is `category: description, budget [features]`.

2. For EACH wishlist item (max 4 — stay within tool-call budget):
   - `web_search "wirecutter site:wirecutter.com <category> <key feature> 2026"` — e.g.
     `wirecutter site:wirecutter.com Chromebook Snapdragon 2026`
   - Also `web_search "<item description> deal sale 2026"` for current price drops.
   - Capture: product name, current price (if surfaced), Wirecutter URL,
     match-quality (Strong / Partial / Weak — your judgement) against the
     wishlist constraints.

3. Build the digest:
   - **Strong matches first** (something Wirecutter recommends that hits the
     budget and feature list).
   - Skip partials/weaks if there's already a strong match for the same item.
   - For items with NO match: include a single line "No new matches this week"
     so James knows the search ran.

4. `telegram_send_message` to chat 5538967144:

   ```
   🛒 Wirecutter — your wishlist this week

   📱 Chromebook (Snapdragon X Plus, $700-1000)
   ✅ <product name> — $X, <wirecutter pick reason>
      <wirecutter URL>

   🎧 Headphones (ANC, $300-500)
   • No new matches this week

   🌊 Wetsuit (4/3, $250-400)
   ⚠️  <product name> — $X (slightly out of budget at $X)
      <url>
   ```

5. If `telegram_send_message` fails, fall back to `send_email` with the same
   body (subject: `🛒 Wirecutter — your wishlist [date]`).

## Guardrails

- Stay under 6 tool calls total (1 read + up to 4 searches + 1 send).
- Wirecutter sometimes 503s under load — if a search returns no results,
  don't retry, just mark it "No match" and move on.
- If `wishlist.md` is missing or empty (`## Items` section empty), send ONE
  message: "Wishlist empty — add items to /lfs/memory/wishlist.md and I'll
  start hunting." Do NOT fall back to generic deals.
- Match quality is YOUR call. Err on the side of fewer-but-better matches.
  An unrelated headphone is worse than "no match this week."

## How James curates

`/lfs/memory/wishlist.md` is hand-maintained. He adds an item when he's
thinking about buying it; removes it (or moves to `# Bought`) when decided.

## Last run
- (firmware tracks `last_run` in cron.json — no manual log here)
