#!/usr/bin/env bash
# read-log.sh — context-bounded read of a rotated Lango log
#
# WHY: even with monitor.sh's per-file cap at 300 KB, an automated tool
# that `cat`s an entire rotated set (8 × 300 KB = ~2.7 MB) would eat ~600 K
# tokens of an AI context window. This wrapper enforces a HARD per-call
# byte budget no matter which file (or how many) you ask for.
#
# Default: last 50 KB of the active log (≈ 12 K tokens — safe single-pass
# Read, leaves lots of context room). Override via --bytes / --lines / --all.
# Bytes budget is clamped to LANGO_READ_MAX_BYTES (default 200 KB).
#
# Usage:
#   scripts/read-log.sh                       # tail last 50 KB of active log
#   scripts/read-log.sh --tail 200            # last 200 lines
#   scripts/read-log.sh --bytes 100000        # last 100 KB (≤ budget)
#   scripts/read-log.sh --grep "cron|brief"   # grep last 200 KB
#   scripts/read-log.sh --grep PAT --all      # grep across all generations
#                                             # (still clamped to budget)
#   scripts/read-log.sh --gen 2               # read rotated file .2 (tail)
#   scripts/read-log.sh --list                # show files + sizes
#
# Output never exceeds the budget. If a query would exceed it, only the
# tail (most recent) portion is returned, with a one-line "[truncated: …]"
# notice prepended.

set -euo pipefail

LOG_DIR="${LANGO_LOG_DIR:-/tmp/lango_logs}"
ACTIVE="$LOG_DIR/serial.log"
BUDGET="${LANGO_READ_MAX_BYTES:-204800}"       # 200 KB hard ceiling per call
# Reserve a small margin for the "[truncated]" notice + final newline so the
# total output stays strictly ≤ BUDGET.
TRUNC_NOTICE_RESERVE=128

mode="bytes"
bytes=51200       # default 50 KB
lines=0
gen=0
pattern=""
across_all=0
list=0

while (($#)); do
  case "$1" in
    --bytes)  mode="bytes"; bytes="$2"; shift ;;
    --tail)   mode="lines"; lines="$2"; shift ;;
    --grep)   pattern="$2"; shift ;;
    --gen)    gen="$2"; shift ;;
    --all)    across_all=1 ;;
    --list)   list=1 ;;
    -h|--help) sed -n '1,30p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

if (( list )); then
  ls -laSh "$LOG_DIR" 2>/dev/null | awk 'NR>1 && /serial\.log/' || echo "no logs"
  exit 0
fi

# clamp byte budget
if (( bytes > BUDGET )); then bytes="$BUDGET"; fi

# pick source(s)
if (( across_all )); then
  # newest first, then rotated .1 .2 …
  sources=("$ACTIVE")
  for i in $(seq 1 16); do
    [[ -e "$ACTIVE.$i" ]] && sources+=("$ACTIVE.$i")
  done
elif (( gen > 0 )); then
  src="$ACTIVE.$gen"
  if [[ ! -e "$src" ]]; then echo "no such gen: $src" >&2; exit 1; fi
  sources=("$src")
else
  sources=("$ACTIVE")
fi

if [[ ! -e "${sources[0]}" ]]; then
  echo "no log: ${sources[0]}" >&2
  exit 1
fi

# fetch: lines mode just tails the active source (no cross-file concat).
# bytes/grep modes can span sources (newest→oldest) but stop at budget.
if [[ "$mode" == "lines" && -z "$pattern" && "$across_all" -eq 0 ]]; then
  # line mode is naturally bounded; still clamp final output to budget
  tail -n "$lines" "${sources[0]}" | tail -c "$BUDGET"
  exit 0
fi

# bytes / grep mode — accumulate from newest source, stop at budget
effective_budget=$(( BUDGET - TRUNC_NOTICE_RESERVE ))
remaining="$effective_budget"
[[ -n "$pattern" ]] || remaining="$bytes"   # only --bytes uses the smaller value
if (( remaining > effective_budget )); then remaining="$effective_budget"; fi
truncated=0

# We build the output newest-last (so a terminal reader sees the most
# recent line at the bottom, like tail). Walk sources oldest→newest.
tmpout="$(mktemp -t langoread.XXXXXX)"
trap 'rm -f "$tmpout"' EXIT

# Reverse the order so newest is read first into a buffer, then we reverse
# again on output. Simpler: read newest into a temp, then prepend older
# until budget exhausted.
declare -a captured
for src in "${sources[@]}"; do
  sz=$(stat -f%z "$src" 2>/dev/null || stat -c%s "$src" 2>/dev/null || echo 0)
  if (( sz == 0 )); then continue; fi
  if [[ -n "$pattern" ]]; then
    matched="$(grep -E "$pattern" "$src" || true)"
    [[ -z "$matched" ]] && continue
    mb=${#matched}
    if (( mb > remaining )); then
      # take the tail of matches that fits
      matched="$(printf '%s' "$matched" | tail -c "$remaining")"
      truncated=1
      captured+=("$matched"); remaining=0; break
    fi
    captured+=("$matched"); remaining=$(( remaining - mb ))
  else
    if (( sz <= remaining )); then
      captured+=("$(cat "$src")")
      remaining=$(( remaining - sz ))
    else
      captured+=("$(tail -c "$remaining" "$src")")
      truncated=1; remaining=0; break
    fi
  fi
  (( remaining == 0 )) && break
done

if (( truncated )); then
  echo "[truncated: ${BUDGET} B budget reached; showing most recent]"
fi
# Reverse so oldest-first, newest-last (chronological reading order)
for (( i=${#captured[@]}-1; i>=0; i-- )); do
  printf '%s\n' "${captured[$i]}"
done
