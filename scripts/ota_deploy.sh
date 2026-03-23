#!/usr/bin/env bash
# ota_deploy.sh — build Langoustine and deploy via OTA
#
# Usage:
#   ./scripts/ota_deploy.sh                   # build + deploy to default IP
#   ./scripts/ota_deploy.sh 192.168.0.43      # explicit device IP
#   SKIP_BUILD=1 ./scripts/ota_deploy.sh      # skip build, deploy existing binary
#   OTA_TOKEN=mytoken ./scripts/ota_deploy.sh # with auth token (if set on device)
#
# Prerequisites:
#   - ESP-IDF sourced (or available at ~/esp/esp-idf)
#   - Device reachable at DEVICE_HOST on port 443 (HTTPS)
#   - This Mac and the device on the same network

set -euo pipefail

DEVICE_HOST="${1:-${DEVICE_HOST:-192.168.0.43}}"
SERVE_PORT="${SERVE_PORT:-8765}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"
BINARY="$BUILD_DIR/langoustine.bin"

# ── 1. Build ────────────────────────────────────────────────────────
if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> Building firmware..."
    bash -c "source ~/esp/esp-idf/export.sh 2>/dev/null && cd '$REPO_DIR' && idf.py build"
fi

if [ ! -f "$BINARY" ]; then
    echo "ERROR: binary not found at $BINARY" >&2
    exit 1
fi

BINARY_SIZE=$(wc -c < "$BINARY")
echo "==> Binary: $BINARY ($BINARY_SIZE bytes)"

# ── 2. Find local IP reachable by the device ────────────────────────
MY_IP=""
for iface in en0 en1 en2 eth0; do
    IP=$(ipconfig getifaddr $iface 2>/dev/null || true)
    if [ -n "$IP" ]; then
        MY_IP="$IP"
        break
    fi
done

if [ -z "$MY_IP" ]; then
    echo "ERROR: could not determine local IP address" >&2
    exit 1
fi

BINARY_URL="http://$MY_IP:$SERVE_PORT/langoustine.bin"
echo "==> Serving from $BINARY_URL"

# ── 3. Start temporary HTTP server ─────────────────────────────────
python3 -m http.server "$SERVE_PORT" --directory "$BUILD_DIR" \
    --bind 0.0.0.0 >/dev/null 2>&1 &
SERVER_PID=$!
trap 'echo "==> Stopping HTTP server (pid $SERVER_PID)"; kill $SERVER_PID 2>/dev/null; exit' EXIT INT TERM

sleep 0.5  # give server time to start

# ── 4. Snapshot pre-OTA uptime (to detect genuine reboot) ──────────
PRE_UPTIME=$(curl -sk --connect-timeout 5 "https://$DEVICE_HOST/api/sysinfo" \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime_s'])" 2>/dev/null || echo "0")
echo "==> Pre-OTA uptime: ${PRE_UPTIME}s"

# ── 5. Trigger OTA ─────────────────────────────────────────────────
echo "==> Triggering OTA on https://$DEVICE_HOST/api/ota"

AUTH_ARGS=()
if [ -n "${OTA_TOKEN:-}" ]; then
    AUTH_ARGS=(-H "Authorization: Bearer $OTA_TOKEN")
fi

RESPONSE=$(curl -ks --connect-timeout 10 -X POST \
    "https://$DEVICE_HOST/api/ota" \
    -H "Content-Type: application/json" \
    "${AUTH_ARGS[@]+"${AUTH_ARGS[@]}"}" \
    -d "{\"url\":\"$BINARY_URL\"}" \
    2>&1) || { echo "ERROR: curl failed — is device reachable at $DEVICE_HOST?" >&2; exit 1; }

echo "==> Device response: $RESPONSE"

if ! echo "$RESPONSE" | grep -q '"ok":true'; then
    echo "ERROR: OTA rejected by device" >&2
    exit 1
fi

# ── 6. Poll /api/ota/status until rebooting, then confirm comeback ──
# Account for: up to 30s agent-idle wait + download + 3s reboot grace
MAX_WAIT=$((BINARY_SIZE / 50000 + 50))   # rough: ~50 KB/s + agent wait + margin
echo "==> Polling OTA status (up to ${MAX_WAIT}s)..."

REBOOTING=0
for i in $(seq 1 $((MAX_WAIT / 4))); do
    # Single request per poll — avoid duplicate TLS handshakes that steal
    # CPU from the OTA download on the ESP32's Core 0.
    POLL_JSON=$(curl -sk --connect-timeout 3 "https://$DEVICE_HOST/api/ota/status" 2>/dev/null || echo '{}')
    STATUS=$(echo "$POLL_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('state','?'))" 2>/dev/null || echo "unreachable")
    PCT=$(echo "$POLL_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('progress_pct',0))" 2>/dev/null || echo "0")
    printf "\r    status=%-12s pct=%3s%%  " "$STATUS" "$PCT"
    if [ "$STATUS" = "rebooting" ]; then
        REBOOTING=1
        echo ""
        echo "==> Device entering reboot..."
        break
    fi
    if [ "$STATUS" = "error" ]; then
        ERR=$(echo "$POLL_JSON" \
            | python3 -c "import sys,json; print(json.load(sys.stdin).get('error_msg','unknown'))" 2>/dev/null || echo "unknown")
        echo ""
        echo "ERROR: OTA failed on device: $ERR" >&2
        exit 1
    fi
    sleep 4
done
echo ""

if [ "$REBOOTING" = "0" ]; then
    # Fallback: check if device already rebooted (uptime < pre-OTA uptime)
    POST_UPTIME=$(curl -sk --connect-timeout 5 "https://$DEVICE_HOST/api/sysinfo" \
        | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime_s'])" 2>/dev/null || echo "-1")
    if [ "$POST_UPTIME" -ge 0 ] && [ "$POST_UPTIME" -lt "$PRE_UPTIME" ] 2>/dev/null; then
        echo "==> Device already rebooted (uptime ${POST_UPTIME}s < pre-OTA ${PRE_UPTIME}s). OTA complete."
        exit 0
    fi
    echo "WARNING: OTA did not reach rebooting state within ${MAX_WAIT}s — check serial console" >&2
    exit 1
fi

# ── 7. Wait for device to come back with lower uptime ───────────────
echo "==> Waiting for device to reboot and come back online..."
for i in $(seq 1 40); do
    NEW_UPTIME=$(curl -sk --connect-timeout 2 "https://$DEVICE_HOST/api/sysinfo" \
        | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime_s'])" 2>/dev/null || echo "-1")
    if [ "$NEW_UPTIME" -ge 0 ] && [ "$NEW_UPTIME" -lt "$PRE_UPTIME" ] 2>/dev/null; then
        echo ""
        echo "==> Device is back online (uptime=${NEW_UPTIME}s). OTA complete."
        exit 0
    fi
    printf "."
    sleep 2
done

echo ""
echo "WARNING: device did not come back within 80s — check serial console"
exit 1
