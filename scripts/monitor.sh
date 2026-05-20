#!/usr/bin/env bash
# monitor.sh — safe Lango serial capture
#
# WHY: ad-hoc background `python -c serial.Serial(...)` readers were getting
# stacked across sessions (5+ at once), all writing to the same log file,
# which inflated /tmp to 159 GB before being noticed. This wraps capture
# behind a single PID lock and a size-rotated log so it can never run away.
#
# Behaviour:
#   - Refuses to start if another monitor is already attached (use --force to
#     kill predecessors and take over).
#   - Rotates log at $MAX_BYTES (default 50 MB) keeping last $KEEP files
#     (default 3) → hard ceiling 200 MB total no matter how long it runs.
#   - Auto-detects the live port (usbserial-* preferred over SLAB; the SLAB
#     bridge has historically returned 0 bytes).
#   - Foreground by default; `--bg` to daemonize.
#
# Usage:
#   scripts/monitor.sh                    # tail port → /tmp/lango_logs/serial.log
#   scripts/monitor.sh --bg               # background (writes pid file)
#   scripts/monitor.sh --force            # kill any existing reader, take over
#   scripts/monitor.sh --port /dev/...    # override port
#   scripts/monitor.sh --stop             # stop a backgrounded monitor

set -euo pipefail

LOG_DIR="${LANGO_LOG_DIR:-/tmp/lango_logs}"
LOG_FILE="$LOG_DIR/serial.log"
PID_FILE="$LOG_DIR/monitor.pid"
MAX_BYTES="${LANGO_LOG_MAX_BYTES:-52428800}"   # 50 MB
KEEP="${LANGO_LOG_KEEP:-3}"
BAUD="${LANGO_BAUD:-115200}"

mkdir -p "$LOG_DIR"

BG=0; FORCE=0; PORT=""; STOP=0
while (($#)); do
  case "$1" in
    --bg)    BG=1 ;;
    --force) FORCE=1 ;;
    --stop)  STOP=1 ;;
    --port)  PORT="$2"; shift ;;
    -h|--help) sed -n '1,30p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

# Detection patterns. We can't grep for "serial.Serial" because we pipe the
# script in via stdin (heredoc) — that string isn't in argv. The LOG_FILE path
# IS in argv, so use that, plus a PID-file as the primary signal.
RUNNING_GREP="$LOG_FILE"     # appears in python argv
LEGACY_GREP="serial\.Serial.*tty\."   # old ad-hoc readers from before this script

if (( STOP )); then
  if [[ -f "$PID_FILE" ]]; then
    pid="$(cat "$PID_FILE")"
    if kill -0 "$pid" 2>/dev/null; then kill "$pid"; echo "stopped pid $pid"; fi
    rm -f "$PID_FILE"
  fi
  # belt-and-braces: kill any orphan readers (this script's + legacy ad-hoc)
  pkill -f "$RUNNING_GREP" 2>/dev/null || true
  pkill -f "$LEGACY_GREP"  2>/dev/null || true
  echo "done"
  exit 0
fi

# --- guard against stacked readers --------------------------------------------
running_pids=""
# 1. PID file from a prior --bg run
if [[ -f "$PID_FILE" ]]; then
  pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    running_pids+=" $pid"
  else
    rm -f "$PID_FILE"   # stale
  fi
fi
# 2. Any python reader (this script's heredoc reader, identified by LOG_FILE path)
adopt="$(pgrep -f "$RUNNING_GREP" 2>/dev/null || true)"
[[ -n "$adopt" ]] && running_pids+=" $adopt"
# 3. Legacy ad-hoc readers from prior sessions (pre-this-script style)
legacy="$(pgrep -f "$LEGACY_GREP" 2>/dev/null || true)"
[[ -n "$legacy" ]] && running_pids+=" $legacy"

# de-dup
running_pids="$(echo "$running_pids" | tr ' ' '\n' | sort -u | xargs)"

if [[ -n "$running_pids" ]]; then
  if (( FORCE )); then
    echo "killing existing reader pid(s): $running_pids"
    # shellcheck disable=SC2086
    kill $running_pids 2>/dev/null || true
    sleep 1
    # nuke survivors
    pkill -f "$RUNNING_GREP" 2>/dev/null || true
    pkill -f "$LEGACY_GREP"  2>/dev/null || true
    rm -f "$PID_FILE"
  else
    echo "ERROR: another serial reader is already attached (pid(s):$running_pids)" >&2
    ps -p $running_pids -o pid,etime,command 2>/dev/null >&2 || true
    echo "use --force to take over, or --stop to terminate." >&2
    exit 1
  fi
fi

# --- pick port ----------------------------------------------------------------
if [[ -z "$PORT" ]]; then
  # Prefer usbserial-* (CP2102 enumerated); SLAB_USBtoUART has historically
  # returned 0 bytes on this board.
  for cand in /dev/tty.usbserial-* /dev/tty.SLAB_USBtoUART; do
    [[ -e "$cand" ]] && PORT="$cand" && break
  done
fi
if [[ -z "$PORT" || ! -e "$PORT" ]]; then
  echo "ERROR: no serial port found (tried /dev/tty.usbserial-* and SLAB)." >&2
  exit 1
fi

echo "monitor: port=$PORT log=$LOG_FILE rotate=${MAX_BYTES}B keep=$KEEP"

# --- the reader (size-rotated, never appends past MAX_BYTES) ------------------
PYTHON="$(command -v python3 || command -v python)"
reader() {
  "$PYTHON" - "$PORT" "$LOG_FILE" "$MAX_BYTES" "$KEEP" "$BAUD" <<'PYEOF'
import os, sys, time, datetime, serial
port, logp, max_bytes, keep, baud = sys.argv[1:6]
max_bytes = int(max_bytes); keep = int(keep); baud = int(baud)

def rotate():
    # serial.log.3 → drop;  serial.log.2 → .3;  .1 → .2;  current → .1
    for i in range(keep, 0, -1):
        src = f"{logp}.{i}" if i > 1 else logp
        dst = f"{logp}.{i if i==keep else i+1}"  # cap at keep
        if i == keep:
            try: os.remove(f"{logp}.{keep}")
            except FileNotFoundError: pass
            continue
        try: os.rename(src, dst)
        except FileNotFoundError: pass
    try: os.rename(logp, f"{logp}.1")
    except FileNotFoundError: pass

while True:
    try:
        s = serial.Serial(port, baud, timeout=1)
        break
    except Exception as e:
        print(f"open fail ({e}), retrying in 5s", file=sys.stderr)
        time.sleep(5)

f = open(logp, "a", buffering=1)
f.write(f"\n=== monitor start {datetime.datetime.now().isoformat()} port={port} ===\n")

while True:
    try:
        line = s.readline().decode("utf-8", "replace").rstrip()
        if line:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            f.write(f"{ts} {line}\n")
            if f.tell() >= max_bytes:
                f.close(); rotate(); f = open(logp, "a", buffering=1)
    except serial.SerialException as e:
        f.write(f"SerialException: {e} — reopening in 5s\n")
        time.sleep(5)
        try: s.close()
        except Exception: pass
        try: s = serial.Serial(port, baud, timeout=1)
        except Exception: time.sleep(5)
PYEOF
}

if (( BG )); then
  ( reader >/dev/null 2>&1 ) &
  echo $! > "$PID_FILE"
  echo "backgrounded pid $(cat "$PID_FILE")  log=$LOG_FILE"
else
  reader
fi
