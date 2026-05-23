#!/usr/bin/env bash
# sonos_watch.sh — long-running ping watcher to catch INTERMITTENT Sonos
# dropouts and timestamp them.
#
# Sonos dropouts on a congested 2.4 GHz band are bursty: a single ping
# snapshot looks fine, then 15 min later it's 10% loss. This loops a short
# ping burst across the Sonos units + control targets every $INTERVAL and
# logs a line ONLY when something is degraded (so the log stays tiny and
# every line is a real event). One "still ok" heartbeat every $HEARTBEAT.
#
# Hard size cap: $MAX_BYTES x 2 rotations. Single-instance PID lock.
#
# Usage:
#   scripts/sonos_watch.sh            # foreground
#   scripts/sonos_watch.sh --bg       # background (pid file)
#   scripts/sonos_watch.sh --stop     # stop a backgrounded watcher
#   scripts/sonos_watch.sh --report   # summarise events seen so far

set -euo pipefail

LOG_DIR="${SONOS_LOG_DIR:-/tmp/lango_logs}"
LOG="$LOG_DIR/sonos_watch.log"
PID_FILE="$LOG_DIR/sonos_watch.pid"
MAX_BYTES="${SONOS_LOG_MAX_BYTES:-262144}"   # 256 KB x 2 = 512 KB ceiling
INTERVAL="${SONOS_WATCH_INTERVAL:-20}"        # seconds between sweeps
HEARTBEAT="${SONOS_WATCH_HEARTBEAT:-1800}"    # "still ok" line every 30 min
BAD_LOSS=1                                    # % loss that counts as degraded
BAD_AVG=60                                    # ms avg latency that counts as degraded

# target list: "label:ip" — edit if IPs change
TARGETS=(
  "gateway:192.168.0.1"          # control — wired/router baseline
  "MainBed-wifi:192.168.0.12"    # newer unit, now in Main Bedroom, WIRELESS (post-swap)
  "Garage-wired:192.168.0.81"    # older unit, now in Garage, WIRED (post-swap)
  "Office:192.168.0.73"          # Sonos — WIRED
  "Bathroom:192.168.0.92"        # Sonos — wireless on SonosNet ch11
  "ESP32:192.168.0.44"           # 2.4GHz canary (Deco 2.4GHz, ch3)
)

mkdir -p "$LOG_DIR"

BG=0; STOP=0; REPORT=0
while (($#)); do
  case "$1" in
    --bg) BG=1 ;;
    --stop) STOP=1 ;;
    --report) REPORT=1 ;;
    -h|--help) sed -n '1,22p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

if (( STOP )); then
  if [[ -f "$PID_FILE" ]]; then
    pid="$(cat "$PID_FILE")"
    kill "$pid" 2>/dev/null && echo "stopped pid $pid" || echo "not running"
    rm -f "$PID_FILE"
  else
    echo "no pid file — not running"
  fi
  exit 0
fi

if (( REPORT )); then
  if [[ ! -f "$LOG" ]]; then echo "no log yet at $LOG"; exit 0; fi
  echo "=== sonos_watch events ($(grep -c DEGRADED "$LOG" 2>/dev/null || echo 0) degraded sweeps) ==="
  grep -E "DEGRADED|heartbeat|start" "$LOG" | tail -40
  exit 0
fi

# single-instance guard
if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
  echo "ERROR: sonos_watch already running (pid $(cat "$PID_FILE")). Use --stop first." >&2
  exit 1
fi

rotate_if_big() {
  local sz
  sz=$(stat -f%z "$LOG" 2>/dev/null || stat -c%s "$LOG" 2>/dev/null || echo 0)
  if (( sz >= MAX_BYTES )); then
    rm -f "$LOG.1"
    mv "$LOG" "$LOG.1"
  fi
}

watcher() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') start — watching ${#TARGETS[@]} targets every ${INTERVAL}s" >> "$LOG"
  local last_hb=0
  while true; do
    local now ts degraded line
    now=$(date +%s)
    ts=$(date '+%H:%M:%S')
    degraded=0
    line=""
    for t in "${TARGETS[@]}"; do
      local lbl="${t%%:*}" ip="${t##*:}"
      # 10 quick packets, 4s ceiling
      local out loss avg
      out=$(ping -c 10 -i 0.2 -t 4 "$ip" 2>/dev/null || true)
      loss=$(echo "$out" | grep -oE '[0-9.]+% packet loss' | grep -oE '^[0-9.]+' || echo 100)
      avg=$(echo "$out"  | awk -F'/' '/round-trip|min.avg/ {print int($5)}')
      [[ -z "$avg" ]] && avg=9999
      # integer-compare loss (may be like 10.0)
      local loss_int="${loss%%.*}"
      if (( loss_int >= BAD_LOSS )) || (( avg >= BAD_AVG )); then
        degraded=1
        line+=" ${lbl}=loss${loss}%/${avg}ms"
      fi
    done
    rotate_if_big
    if (( degraded )); then
      echo "$ts DEGRADED$line" >> "$LOG"
    elif (( now - last_hb >= HEARTBEAT )); then
      echo "$ts heartbeat — all targets ok" >> "$LOG"
      last_hb=$now
    fi
    sleep "$INTERVAL"
  done
}

if (( BG )); then
  watcher &
  echo $! > "$PID_FILE"
  echo "sonos_watch backgrounded pid $(cat "$PID_FILE") — log: $LOG"
  echo "check anytime:  scripts/sonos_watch.sh --report"
  echo "stop:           scripts/sonos_watch.sh --stop"
else
  watcher
fi
