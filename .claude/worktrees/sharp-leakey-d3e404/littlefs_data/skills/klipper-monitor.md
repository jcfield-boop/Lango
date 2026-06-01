# Klipper Monitor

Check 3D printer status via Moonraker and alert on print completion or errors.

## When to use
- User asks "is the printer running?", "what's the print doing?", "check the printer"
- Triggered by cron to monitor an active print (check every 15–30 min during a print)
- Before leaving home ("is it safe to leave?")

## Moonraker API endpoints used
- `GET /printer/objects/query?print_stats` — print state, filename, progress, duration
- `GET /printer/objects/query?heater_bed,extruder` — temperatures
- `GET /server/history/list?limit=1` — last completed job

## Steps

### Status check (manual query)
1. Call klipper: `{"method":"GET","endpoint":"/printer/objects/query?print_stats"}`
2. Call klipper: `{"method":"GET","endpoint":"/printer/objects/query?heater_bed,extruder"}`
3. Parse `print_stats.state`:
   - `printing` → report: filename, progress %, time elapsed, time remaining estimate
   - `paused` → report: paused at X% — user action required
   - `standby` / `complete` → report: printer idle, last print info if available
   - `error` → **ALERT** — include error message
4. Report temperatures: bed actual/target, extruder actual/target

### Print progress estimate
- `time_elapsed` (seconds) + `progress` (0.0–1.0) → estimated total: `elapsed / progress`
- Remaining: `total - elapsed`, formatted as "Xh Ym remaining"

### Alert conditions (send Telegram message)
- State transitions to `complete` → "✅ Print complete: <filename>"
- State transitions to `error` → "🔴 Printer ERROR: <error message>"
- State = `paused` → "⏸ Print paused at X% — needs attention"
- Extruder temp > 250°C unexpectedly → "⚠️ Extruder overtemp: X°C"
- Bed temp > 120°C unexpectedly → "⚠️ Bed overtemp: X°C"

## Output format (manual check)
```
🖨️ Printer Status
State: printing
File: benchy.gcode
Progress: 67% (2h 14m elapsed, ~1h 06m remaining)
Extruder: 215°C / 215°C  Bed: 60°C / 60°C
```

## Cron monitor pattern
If called by cron mid-print:
1. Check print_stats
2. If `complete` or `error` → send Telegram alert + remove/disable the monitor cron job
3. If still `printing` → log progress silently, no alert
4. If `standby` and no cron was expected → skip silently

## Notes
- Moonraker at `http://192.168.0.50:7125` (from SERVICES.md)
- No API key required for local network access
- Use klipper tool with `endpoint` starting with `/`
