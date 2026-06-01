#!/usr/bin/env python3
"""
lango_relay.py — Claude↔ESP32 outbox relay

Watches ~/Lango/outbox.json for items written by Claude (Cowork scheduled tasks)
and POSTs each item to http://192.168.0.44/api/relay so the ESP32 can send them
via its own SMTP and Telegram credentials.

Outbox format — one JSON object per item in a top-level array:
  [
    {"type": "email",    "to": "jcfield@gmail.com", "subject": "...", "body": "..."},
    {"type": "telegram", "chat_id": "5538967144",   "text": "..."}
  ]

The relay atomically swaps the file: it reads, clears, then dispatches.
Failed items are written back to outbox.json for retry on the next poll.

Install as a launchd agent — see com.lango.relay.plist alongside this script.
"""

import json
import os
import sys
import time
import urllib.request
import urllib.error
import logging
from pathlib import Path

# ── Config ──────────────────────────────────────────────────────────────────

OUTBOX      = Path.home() / "Lango" / "outbox.json"
DEVICE_URL  = "http://192.168.0.44/api/relay"
POLL_S      = 5          # seconds between outbox checks
RETRY_S     = 30         # seconds before retrying a failed item
TIMEOUT_S   = 15         # HTTP request timeout
LOG_FILE    = Path.home() / "Library" / "Logs" / "lango_relay.log"

# Auth token — must match NVS key "ws_config/auth_token" on the device.
# Set via: lango> set_auth_token <token>
# Leave empty if auth is disabled on the device (default).
AUTH_TOKEN  = os.environ.get("LANGO_AUTH_TOKEN", "")

# ── Logging ──────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler(sys.stdout),
    ],
)
log = logging.getLogger("relay")

# ── Helpers ──────────────────────────────────────────────────────────────────

def _read_outbox() -> list:
    """Read and atomically clear the outbox. Returns list of items."""
    if not OUTBOX.exists():
        return []
    try:
        with open(OUTBOX, "r") as f:
            data = f.read().strip()
        if not data:
            return []
        items = json.loads(data)
        if not isinstance(items, list):
            log.warning("outbox.json is not a JSON array — skipping")
            return []
        # Clear immediately so we don't re-process on crash
        OUTBOX.write_text("[]")
        return items
    except (json.JSONDecodeError, OSError) as e:
        log.error("Failed to read outbox: %s", e)
        return []


def _write_back(items: list):
    """Write failed items back to outbox for retry."""
    try:
        existing = []
        if OUTBOX.exists():
            try:
                existing = json.loads(OUTBOX.read_text().strip() or "[]")
            except json.JSONDecodeError:
                pass
        OUTBOX.write_text(json.dumps(existing + items, indent=2))
    except OSError as e:
        log.error("Failed to write back failed items: %s", e)


def _post(item: dict) -> bool:
    """POST a single item to /api/relay. Returns True on success."""
    body = json.dumps(item).encode()
    headers = {"Content-Type": "application/json"}
    if AUTH_TOKEN:
        headers["Authorization"] = f"Bearer {AUTH_TOKEN}"
    req = urllib.request.Request(DEVICE_URL, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
            result = json.loads(resp.read())
            if result.get("ok"):
                item_type = item.get("type", "?")
                label = item.get("subject") or item.get("text", "")[:60]
                log.info("relay OK  [%s] %s", item_type, label)
                return True
            else:
                log.warning("relay nok [%s]: %s", item.get("type"), result)
                return False
    except urllib.error.URLError as e:
        log.warning("relay fail: %s — will retry", e)
        return False
    except Exception as e:
        log.error("relay error: %s", e)
        return False


# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    log.info("lango_relay started — watching %s → %s", OUTBOX, DEVICE_URL)
    OUTBOX.parent.mkdir(parents=True, exist_ok=True)
    if not OUTBOX.exists():
        OUTBOX.write_text("[]")

    while True:
        items = _read_outbox()
        if items:
            log.info("outbox: %d item(s) to relay", len(items))
            failed = []
            for item in items:
                if not _post(item):
                    failed.append(item)
            if failed:
                log.warning("%d item(s) failed — writing back for retry in %ds",
                            len(failed), RETRY_S)
                _write_back(failed)
                time.sleep(RETRY_S)
            else:
                time.sleep(POLL_S)
        else:
            time.sleep(POLL_S)


if __name__ == "__main__":
    main()
