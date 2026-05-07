# Home Assistant Updater

Daily check for available HA updates. Auto-applies safe ones (integrations/devices),
notifies via Telegram for risky ones (HA Core — requires restart).

## When to use
Fired from cron job `haupd001` daily at 09:00 PDT. Can also be invoked manually
("check for HA updates").

## Policy

| Update type                        | Action                        |
|------------------------------------|-------------------------------|
| `update.home_assistant_core_update`| Notify only — needs restart   |
| `update.home_assistant_operating_system_update` | Notify only — host reboot |
| `update.home_assistant_supervisor_update` | Blocked by firmware (hassio endpoint) |
| Integration updates (HACS / core integrations) | **Auto-install**  |
| Device firmware (Zigbee / Z-Wave / ESPHome) | **Auto-install**     |

## Steps

1. **Check HA Core update** — `ha_request GET /api/states/update.home_assistant_core_update`.
   If `state == "on"`, record: current version, latest version, release notes URL.
   Do NOT auto-install.

2. **Check OS update** (only if supervisor install) —
   `ha_request GET /api/states/update.home_assistant_operating_system_update`.
   If `state == "on"`, record current/latest. Do NOT auto-install.

3. **Check integration updates** — probe the common ones:
   - `update.hacs_update`
   - `update.home_assistant_google_translate_update`
   - `update.esphome_update` (if you run ESPHome)
   - Any user-configured list in `/lfs/config/ha_updates.md` (one entity_id per line)

   For each entity whose `state == "on"`:
   - `ha_request POST /api/services/update/install` body `{"entity_id": "update.X"}`
   - Record success/failure.

4. **Notifications — quiet by default; only notify when there's actually something for James to do.**

   **(a) Email — ONLY if a Core or OS update is available (manual action required).**
   No email when everything is already current, no email when only auto-installs ran. James asked
   2026-05-02: "the email about HA updates only needs to be sent if there are updates that need
   applying." `send_email`:
   - to: `jcfield@gmail.com`
   - subject: `⚠️ Home Assistant manual update required — [type]`
   - body (under 300 words, no markdown):
     ```
     Home Assistant has an update that needs your attention.

     [Core] Current: X.Y.Z  →  Latest: A.B.C
            Release notes: <url if available>
            Action: open HA → Settings → System → Updates. Backup first.
            Reply "yes install HA" in Telegram to trigger install from Lango (still needs restart).

     [OS]   Current: X.Y.Z  →  Latest: A.B.C
            Action: host reboot required. Install via Settings → System → Hardware.

     Integration updates already applied (if any): <list>
     ```

   **(b) Telegram — ONLY if something happened that James should know about:**
   - Core/OS update available → "⚠️ HA Core X.Y.Z → A.B.C — email sent (manual install needed)."
   - Auto-installs ran → "✅ Installed: <entity1>, <entity2>"
   - All current → **stay silent. NO Telegram message.** James doesn't need a daily "all good" ping.
   - Errors (auth fail, partial install fail, etc.) → "🔴 HA update: <error>"

   **(c) If `send_email` fails on a Core/OS notification**, fall back to a long Telegram message
   (same body as the email) so James still gets the full detail.

## Guardrails

- NEVER auto-install `update.home_assistant_core_update` — risk of failed restart.
- NEVER call `/api/services/homeassistant/restart` without explicit user command.
- If `ha_request` returns 401/403: the HA token is invalid. Send a Telegram alert and stop.
- If more than 3 integrations have updates: install sequentially, not in parallel (HA locks the update queue).
- If a single `update.install` call fails: log it, continue with the rest, report in summary.

## Customization
Edit `/lfs/config/ha_updates.md` to add specific entity IDs you want checked.
One per line, comments with `#`. Example:
```
# Core integrations worth watching
update.hacs_update
update.z2m_update
update.esphome_update
```
