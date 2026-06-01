#!/usr/bin/env python3
"""
lango_send.py — CLI helper to queue a message into outbox.json

Usage (from Cowork bash or terminal):
  # Send email:
  python3 ~/Lango/scripts/lango_send.py email "Subject here" "Body text" [to@email.com]

  # Send Telegram:
  python3 ~/Lango/scripts/lango_send.py telegram "Message text" [chat_id]

The relay daemon (lango_relay.py) picks up the outbox within 5 seconds.

Claude uses this script from the Cowork bash tool to send messages via the
ESP32 without needing direct LAN access from the sandbox.
"""

import json
import sys
from pathlib import Path

OUTBOX       = Path.home() / "Lango" / "outbox.json"
DEFAULT_TO   = "jcfield@gmail.com"
DEFAULT_CHAT = "5538967144"


def _load() -> list:
    if OUTBOX.exists():
        try:
            return json.loads(OUTBOX.read_text().strip() or "[]")
        except json.JSONDecodeError:
            pass
    return []


def _save(items: list):
    OUTBOX.parent.mkdir(parents=True, exist_ok=True)
    OUTBOX.write_text(json.dumps(items, indent=2))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    kind = sys.argv[1].lower()
    items = _load()

    if kind == "email":
        if len(sys.argv) < 4:
            print("Usage: lango_send.py email <subject> <body> [to]")
            sys.exit(1)
        subject = sys.argv[2]
        body    = sys.argv[3]
        to      = sys.argv[4] if len(sys.argv) > 4 else DEFAULT_TO
        items.append({"type": "email", "to": to, "subject": subject, "body": body})
        print(f"Queued email → {to}: {subject[:60]}")

    elif kind == "telegram":
        text    = sys.argv[2]
        chat_id = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_CHAT
        items.append({"type": "telegram", "chat_id": chat_id, "text": text})
        print(f"Queued telegram → {chat_id}: {text[:60]}")

    else:
        print(f"Unknown type '{kind}' — use 'email' or 'telegram'")
        sys.exit(1)

    _save(items)


if __name__ == "__main__":
    main()
