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

DEVICE_HOST="${1:-${DEVICE_HOST:-192.168.0.44}}"
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
# Use socketserver (multi-connection) so the ESP32 can reconnect on retry
# without the server hanging on a kept-alive single-threaded socket.
python3 -c "
import http.server, socketserver, os, sys
os.chdir(sys.argv[1])
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('0.0.0.0', int(sys.argv[2])), H) as s:
    s.serve_forever()
" "$BUILD_DIR" "$SERVE_PORT" &
SERVER_PID=$!
trap 'echo "==> Stopping HTTP server (pid $SERVER_PID)"; kill $SERVER_PID 2>/dev/null; exit' EXIT INT TERM

# Wait until server is actually accepting connections (up to 5s)
echo "==> Waiting for HTTP server on port $SERVE_PORT..."
for i in $(seq 1 10); do
    CODE=$(curl -s --connect-timeout 1 "http://127.0.0.1:$SERVE_PORT/langoustine.bin" \
        -o /dev/null -w "%{http_code}" 2>/dev/null || echo "0")
    [ "$CODE" = "200" ] && break
    sleep 0.5
done
echo "==> HTTP server ready"

# ── 4. Snapshot pre-OTA uptime (to detect genuine reboot) ──────────
PRE_UPTIME=$(curl -s --connect-timeout 5 "http://$DEVICE_HOST/api/sysinfo" \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime_s'])" 2>/dev/null || echo "0")
echo "==> Pre-OTA uptime: ${PRE_UPTIME}s"

# ── 5. Trigger OTA ─────────────────────────────────────────────────
echo "==> Triggering OTA on http://$DEVICE_HOST/api/ota"

AUTH_ARGS=()
if [ -n "${OTA_TOKEN:-}" ]; then
    AUTH_ARGS=(-H "Authorization: Bearer $OTA_TOKEN")
fi

RESPONSE=$(curl -ks --connect-timeout 10 --max-time 15 -X POST \
    "http://$DEVICE_HOST/api/ota" \
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
# ESP32 OTA via WiFi typically runs at 10-20 KB/s due to OTA buffer and yield overhead.
# Allow ~30 KB/s pessimistic + 90s margin for agent-idle wait + reboot grace.
MAX_WAIT=$((BINARY_SIZE / 10000 + 90))   # ~10 KB/s pessimistic + generous margin
echo "==> Polling OTA status (up to ${MAX_WAIT}s)..."

REBOOTING=0
OTA_ATTEMPT=1
POLL_START=$(date +%s)

while true; do
    ELAPSED=$(( $(date +%s) - POLL_START ))
    if [ "$ELAPSED" -ge "$MAX_WAIT" ]; then
        echo ""
        echo "WARNING: OTA did not complete within ${MAX_WAIT}s — check device" >&2
        break
    fi

    # Single request per poll — avoid concurrent TLS handshakes that steal
    # CPU from the OTA download on the ESP32's Core 0.
    POLL_JSON=$(curl -s --connect-timeout 3 "http://$DEVICE_HOST/api/ota/status" 2>/dev/null || echo '{}')
    STATUS=$(echo "$POLL_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('state','?'))" 2>/dev/null || echo "unreachable")
    PCT=$(echo "$POLL_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('progress_pct',0))" 2>/dev/null || echo "0")
    printf "\r    status=%-12s pct=%3s%%  elapsed=%ds  " "$STATUS" "$PCT" "$ELAPSED"

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
        if [ "$OTA_ATTEMPT" -lt 2 ]; then
            OTA_ATTEMPT=2
            echo "==> OTA error: $ERR — retrying (attempt 2)..."
            curl -ks --connect-timeout 10 --max-time 15 -X POST \
                "http://$DEVICE_HOST/api/ota" \
                -H "Content-Type: application/json" \
                "${AUTH_ARGS[@]+"${AUTH_ARGS[@]}"}" \
                -d "{\"url\":\"$BINARY_URL\"}" >/dev/null 2>&1 || true
            sleep 2
            continue
        fi
        echo "ERROR: OTA failed on device (attempt $OTA_ATTEMPT): $ERR" >&2
        exit 1
    fi

    sleep 4
done
echo ""

if [ "$REBOOTING" = "0" ]; then
    # Fallback: device may have rebooted faster than our 4s poll interval caught it.
    # Check if uptime has already reset (common when download is fast at end).
    POST_UPTIME=$(curl -s --connect-timeout 5 "http://$DEVICE_HOST/api/sysinfo" \
        | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime_s'])" 2>/dev/null || echo "-1")
    if [ "$POST_UPTIME" -ge 0 ] && [ "$POST_UPTIME" -lt "$PRE_UPTIME" ] 2>/dev/null; then
        echo "==> Device already rebooted (uptime ${POST_UPTIME}s < pre-OTA ${PRE_UPTIME}s). OTA complete."
        exit 0
    fi
    # If status was "idle" with low uptime we also succeeded (device booted new firmware)
    FINAL_STATUS=$(curl -s --connect-timeout 3 "http://$DEVICE_HOST/api/ota/status" \
        | python3 -c "import sys,json; print(json.load(sys.stdin).get('state','?'))" 2>/dev/null || echo "?")
    if [ "$FINAL_STATUS" = "idle" ] && [ "$POST_UPTIME" -ge 0 ] && [ "$POST_UPTIME" -lt 120 ] 2>/dev/null; then
        echo "==> OTA complete — device online with uptime ${POST_UPTIME}s (new firmware)."
        exit 0
    fi
    echo "WARNING: OTA did not complete within ${MAX_WAIT}s — check device" >&2
    exit 1
fi

# ── 7. Wait for device to come back with lower uptime ───────────────
echo "==> Waiting for device to reboot and come back online..."
for i in $(seq 1 40); do
    NEW_UPTIME=$(curl -s --connect-timeout 2 "http://$DEVICE_HOST/api/sysinfo" \
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
