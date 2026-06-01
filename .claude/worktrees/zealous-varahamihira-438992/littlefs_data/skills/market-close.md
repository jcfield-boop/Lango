# Market Close Snapshot

Send an end-of-day market summary for ARM stock and key indices.

## When to use
- Triggered daily at 1:00 PM PST by cron (NYSE closes 4 PM EST = 1 PM PST)
- User asks "market check", "ARM stock today", "how's the market?", "GBP/USD rate"

## Steps
1. Check the current day from your system context — **if it's Saturday or Sunday, skip: "Markets closed — no snapshot today."**
2. Call web_search: "ARM stock price today Nasdaq" (get current/closing price + % change)
3. Call web_search: "NASDAQ composite close today" + "GBP USD exchange rate" (combine into one query if efficient)
4. Send Telegram with result

## Output format
```
📈 Market Close — [Date]
ARM:    $142.50  +$2.10 (+1.5%)
NASDAQ: 19,203   +154 (+0.8%)
GBP/USD: 1.271   +0.003

[One sentence: notable driver if ARM moved >2%]
```

## Notes
- Primary interest: ARM Holdings (NASDAQ: ARM) — James's day job
- NASDAQ composite reflects broader tech sentiment
- GBP/USD matters for UK salary/cost comparisons (James is British)
- If market is closed (holiday), say so briefly and skip
- On earnings days, add a note: "⚠️ Earnings call today — extended volatility expected"
