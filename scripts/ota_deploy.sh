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

# ── 4. Trigger OTA ─────────────────────────────────────────────────
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

# ── 5. Wait for download to complete, then reboot ───────────────────
echo "==> OTA started. Waiting for device to download (~$((BINARY_SIZE / 50000))s)..."
sleep $((BINARY_SIZE / 50000 + 10))   # rough: ~50 KB/s on local WiFi + margin

echo "==> Waiting for device to reboot and come back online..."
for i in $(seq 1 40); do
    if curl -ks --connect-timeout 2 "https://$DEVICE_HOST/health" >/dev/null 2>&1; then
        echo ""
        echo "==> Device is back online! OTA complete."
        exit 0
    fi
    printf "."
    sleep 2
done

echo ""
echo "WARNING: device did not come back within 80s — check serial console"
exit 1
