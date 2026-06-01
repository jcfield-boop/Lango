#!/bin/bash
# setup_relay.sh — one-shot: push, build, flash, install relay daemon
# Run from anywhere: bash ~/Lango/scripts/setup_relay.sh

set -e
REPO="$HOME/Lango"
PLIST="$REPO/scripts/com.lango.relay.plist"
LAUNCHAGENTS="$HOME/Library/LaunchAgents"

echo "=== 1/4  Git push ==="
cd "$REPO"
git push

echo ""
echo "=== 2/4  Build firmware ==="
source ~/esp/esp-idf/export.sh
idf.py build

echo ""
echo "=== 3/4  Flash device ==="
# Auto-detect the USB serial port
PORT=$(ls /dev/tty.usbserial-* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
    echo "ERROR: No /dev/tty.usbserial-* found. Is the ESP32 connected via USB?"
    exit 1
fi
echo "Flashing to $PORT ..."
idf.py -p "$PORT" flash

echo ""
echo "=== 4/4  Install relay daemon ==="
mkdir -p "$LAUNCHAGENTS"
cp "$PLIST" "$LAUNCHAGENTS/com.lango.relay.plist"

# Unload existing instance if running
launchctl unload "$LAUNCHAGENTS/com.lango.relay.plist" 2>/dev/null || true
launchctl load "$LAUNCHAGENTS/com.lango.relay.plist"

echo ""
echo "✅  Done!"
echo "   Relay daemon running — logs: tail -f ~/Library/Logs/lango_relay.log"
echo "   Test: python3 $REPO/scripts/lango_send.py telegram 'relay test from setup script'"
