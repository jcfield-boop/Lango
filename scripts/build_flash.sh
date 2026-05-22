#!/usr/bin/env bash
# build_flash.sh — build Langoustine firmware and flash to connected ESP32-S3
#
# Usage:
#   bash scripts/build_flash.sh            # build + flash + monitor
#   bash scripts/build_flash.sh --build    # build only
#   bash scripts/build_flash.sh --flash    # flash only (uses existing build)
#   bash scripts/build_flash.sh --monitor  # serial monitor only

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_ONLY=0; FLASH_ONLY=0; MONITOR_ONLY=0
while (($#)); do
  case "$1" in
    --build)   BUILD_ONLY=1 ;;
    --flash)   FLASH_ONLY=1 ;;
    --monitor) MONITOR_ONLY=1 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

# ── Source ESP-IDF ───────────────────────────────────────────────────────────
# NOTE: never pipe `source export.sh` (e.g. `source ... | grep`). A pipeline
# runs each stage in a SUBSHELL, so the PATH / IDF_PYTHON_ENV_PATH exports
# vanish when that subshell exits and `idf.py` is never found. Source it
# directly into the current shell instead.
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_PATH

if ! command -v idf.py >/dev/null 2>&1; then
  if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "[build_flash] ERROR: ESP-IDF export.sh not found at $IDF_PATH/export.sh" >&2
    echo "[build_flash] Set IDF_PATH, or install ESP-IDF under ~/esp/esp-idf" >&2
    exit 1
  fi

  # ESP-IDF's export.sh derives the Python virtualenv name from whatever
  # `python3` is first on PATH (e.g. idf6.0_py3.12_env). If the system
  # python3 has since been upgraded (Homebrew 3.12 -> 3.14) export.sh looks
  # for a venv that was never created and aborts with "virtual environment
  # ... not found". Prepend the ACTUAL installed IDF venv so export.sh
  # detects the matching interpreter. (This also puts a `python` on PATH,
  # which idf.py's `#!/usr/bin/env python` shebang requires.)
  for _venv in "$HOME"/.espressif/python_env/*/bin; do
    if [[ -x "$_venv/python" ]]; then
      export PATH="$_venv:$PATH"
      echo "[build_flash] Using IDF Python venv: $_venv"
      break
    fi
  done

  # export.sh is not written to tolerate `set -euo pipefail`; relax around it.
  # Source directly (NOT in a pipeline) so its exports reach this shell.
  _idf_log="$(mktemp -t idf_export.XXXXXX)"
  set +eu
  # shellcheck disable=SC1090,SC1091
  source "$IDF_PATH/export.sh" >"$_idf_log" 2>&1
  set -eu
fi

# Hard verification — fall back to the tool's full path if PATH still misses it.
if ! command -v idf.py >/dev/null 2>&1; then
  if [[ -f "$IDF_PATH/tools/idf.py" ]]; then
    IDF_PY_BIN="$IDF_PATH/tools/idf.py"
    echo "[build_flash] WARN: idf.py not on PATH after export.sh — using full path" >&2
  else
    echo "[build_flash] ERROR: idf.py not found on PATH or at $IDF_PATH/tools/idf.py" >&2
    [[ -f "${_idf_log:-}" ]] && { echo "[build_flash] --- export.sh output: ---" >&2; cat "$_idf_log" >&2; }
    exit 1
  fi
else
  IDF_PY_BIN="$(command -v idf.py)"
fi
[[ -f "${_idf_log:-/dev/null}" ]] && rm -f "$_idf_log"

# Invoke idf.py through this wrapper everywhere below so the resolved binary
# (PATH entry or full path) is always used.
idf() { "$IDF_PY_BIN" "$@"; }

echo "[build_flash] ESP-IDF: $IDF_PATH"
echo "[build_flash] idf.py:  $IDF_PY_BIN"

# ── Detect serial port ───────────────────────────────────────────────────────
PORT=""
for candidate in /dev/tty.usbserial-* /dev/tty.SLAB_USBtoUART* /dev/cu.usbserial-*; do
  if [[ -e "$candidate" ]]; then
    PORT="$candidate"
    break
  fi
done
if [[ -z "$PORT" && $BUILD_ONLY -eq 0 ]]; then
  echo "[build_flash] ERROR: no USB serial port found — is the ESP32 plugged in?"
  exit 1
fi
[[ -n "$PORT" ]] && echo "[build_flash] Serial port: $PORT"

# ── Monitor only ─────────────────────────────────────────────────────────────
if [[ $MONITOR_ONLY -eq 1 ]]; then
  exec "$IDF_PY_BIN" -p "$PORT" monitor
fi

# ── Build ────────────────────────────────────────────────────────────────────
if [[ $FLASH_ONLY -eq 0 ]]; then
  echo ""
  echo "════════════════════════════════════════"
  echo "  Building Langoustine firmware"
  echo "════════════════════════════════════════"
  idf build
  echo ""
  BIN_SIZE=$(stat -f%z build/langoustine.bin 2>/dev/null || stat -c%s build/langoustine.bin)
  echo "[build_flash] langoustine.bin: $(( BIN_SIZE / 1024 )) KB"
fi

if [[ $BUILD_ONLY -eq 1 ]]; then
  echo "[build_flash] Build complete. Run --flash to flash."
  exit 0
fi

# ── Flash ────────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo "  Flashing to $PORT"
echo "════════════════════════════════════════"
# Use idf.py flash (covers firmware + littlefs partition)
idf -p "$PORT" flash

echo ""
echo "════════════════════════════════════════"
echo "  Flash complete — starting monitor"
echo "  (Ctrl+] to exit)"
echo "════════════════════════════════════════"
echo ""
exec "$IDF_PY_BIN" -p "$PORT" monitor
