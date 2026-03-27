#!/usr/bin/env bash
# lfs_sync.sh — Sync littlefs_data/ to a Langoustine device over HTTP
#
# Usage: bash scripts/lfs_sync.sh [device_ip]
#
# Uploads each file under littlefs_data/ to the device via POST /api/file?path=...
# Skips binary files and files larger than 48KB (device limit).

set -euo pipefail

DEVICE_IP="${1:-192.168.0.44}"
BASE_URL="http://${DEVICE_IP}"
SRC_DIR="littlefs_data"
MAX_SIZE=$((48 * 1024))

if [ ! -d "$SRC_DIR" ]; then
    echo "ERROR: $SRC_DIR not found. Run from the project root."
    exit 1
fi

uploaded=0
skipped=0
failed=0

while IFS= read -r file; do
    rel="${file#$SRC_DIR/}"
    lfs_path="/lfs/${rel}"
    size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)

    if [ "$size" -gt "$MAX_SIZE" ]; then
        echo "SKIP (>48KB): $rel"
        skipped=$((skipped + 1))
        continue
    fi

    # URL-encode the path (minimal: spaces only — LFS paths are usually clean)
    encoded_path=$(printf '%s' "$lfs_path" | sed 's/ /%20/g')

    status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 15 \
        -X POST "${BASE_URL}/api/file?path=${encoded_path}" \
        --data-binary "@${file}")

    if [ "$status" = "200" ]; then
        echo "  OK: $rel ($size bytes)"
        uploaded=$((uploaded + 1))
    else
        echo "FAIL ($status): $rel"
        failed=$((failed + 1))
    fi
done < <(find "$SRC_DIR" -type f | sort)

echo ""
echo "Done: $uploaded uploaded, $skipped skipped, $failed failed"
