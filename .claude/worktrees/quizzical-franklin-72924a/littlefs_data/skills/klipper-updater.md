# Klipper Updater

Daily check for available Klipper / Moonraker / system-package / client updates
via Moonraker's update_manager API. **Notify-only — never auto-installs.**
Klipper updates require a service restart and could disrupt a running print, so
James decides when to pull the trigger.

## When to use
Fired from cron job `kupd0001` daily at 09:30 PDT (30 min after the HA check so
they don't compete for tool slots). Can also be invoked manually
("check for klipper updates", "any printer updates?").

## Policy

| Update target               | Action                       |
|-----------------------------|------------------------------|
| `klipper`                   | Notify only                  |
| `moonraker`                 | Notify only                  |
| `system` packages           | Notify only                  |
| `mainsail` / `fluidd` / clients | Notify only              |
| **Anything**                | **Never auto-install** (printer might be mid-job) |

## Steps

> **One status call. One notification. Done.** Do NOT call `/machine/update/status`
> more than once per turn. Do NOT call `/machine/update/refresh` — Moonraker's
> internal cache is fresh enough (refreshed every hour by Moonraker itself).
> Calling refresh can cause Moonraker to re-check GitHub mid-skill, change
> what's reported, and trick the model into sending a second email about the
> "new" state. (Observed 2026-05-02 first test run — sent two emails.)

1. **Get status (single call):**
   `klipper_request {"method":"GET","endpoint":"/machine/update/status?refresh=false"}`

   Response shape (only the fields we care about):
   ```json
   {
     "result": {
       "version_info": {
         "klipper":   {"version":"v0.12.0", "remote_version":"v0.12.0", "is_dirty":false},
         "moonraker": {"version":"v0.9.3",  "remote_version":"v0.9.4"},
         "mainsail":  {"version":"v2.13.0", "remote_version":"v2.13.0"},
         "system":    {"package_count": 3}     /* ← apt updates pending */
       },
       "busy": false
     }
   }
   ```

3. **Determine what (if anything) needs attention.** A component "needs an
   update" when:
   - For klipper/moonraker/mainsail/fluidd: `version != remote_version` (and
     `is_dirty == false` — dirty checkouts are user-modified and we shouldn't
     touch them).
   - For system: `package_count > 0`.

   If a print is currently active (check `print_stats.state` first), DEFER
   notification entirely — don't distract James while he's printing. The next
   day's cron tick will catch it once the printer is idle.

4. **Notifications — quiet by default; only notify when there's actually
   something for James to do.** Mirrors the ha-updater policy James asked for
   2026-05-02.

   **GOLDEN RULE: AT MOST ONE `send_email` AND AT MOST ONE `telegram_send_message`
   PER TURN.** After the first send_email succeeds, finalise the turn — do NOT
   re-check status, re-fetch anything, or call send_email again. The skill is
   triggered fresh by cron the next day; that is when state changes get a new
   notification. If you find yourself thinking "I should also email about X" —
   X belongs in the SAME single email, not a follow-up.

   **(a) Email — ONLY if klipper itself, moonraker, or system packages need an
   update.** No email when everything is current. No email for client-only
   updates (mainsail/fluidd are in-browser apps, low risk, James can update
   them when he opens the UI).
   - to: `jcfield@gmail.com`
   - subject: `🖨️ Klipper update available — [component]`
   - body (under 250 words, no markdown):
     ```
     Moonraker reports the following updates pending on the printer:

     [klipper]   Current: v0.12.0  →  Latest: v0.12.1
                 Action: open Mainsail → Settings → Software Update.
                 Will require a klipper service restart (not safe mid-print).

     [moonraker] Current: v0.9.3  →  Latest: v0.9.4
                 Action: same UI; restarts moonraker but keeps klipper running.

     [system]    3 apt packages pending.
                 Action: SSH in and `sudo apt upgrade` when convenient.

     Client updates available (low priority): <list>
     Printer state at check time: <idle/printing/paused>
     ```

   **(b) Telegram (chat 5538967144) — ONLY for actionable events:**
   - klipper / moonraker / system update → "🖨️ <component> v<old> → v<new> — email sent"
   - Errors (Moonraker unreachable, auth fail, etc.) → "🔴 Klipper update check: <error>"
   - Everything current → **stay silent. NO Telegram message.**
   - Print active → "📄 Update check skipped — print in progress"

   **(c) If `send_email` fails on a klipper/moonraker/system notification**,
   fall back to a long Telegram message with the full email body so James
   still gets the detail.

## Guardrails

- **NEVER POST to `/machine/update/<component>`** — install endpoints are
  blocked at the firmware level anyway, but explicitly do not attempt.
- **NEVER call `/machine/services/<svc>/restart` or `/machine/shutdown`** —
  also blocked at firmware level. These would disrupt a running print.
- If `klipper_request` returns 401: Moonraker apikey is invalid. Send
  Telegram alert and stop.
- If Moonraker returns "service unavailable" (503) or "update manager busy",
  retry once after 30 s — the refresh can take a minute on a slow Pi.
- Stay under 4 tool calls total (refresh + status + maybe print_stats =
  3 calls; leaves headroom under LANG_AGENT_MAX_TOOL_ITER=10).

## Manual invocation
"check for klipper updates" — same flow, but reply via the originating
channel (Telegram if asked from Telegram, voice if asked from PTT, etc).
The "stay silent if all current" rule does NOT apply to manual queries:
always confirm, even if everything is up to date ("✅ All current — klipper
v0.12.0, moonraker v0.9.3, no system packages pending").
