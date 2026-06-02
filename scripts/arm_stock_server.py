#!/usr/bin/env python3
"""
arm_stock_server.py — fetch ARM stock every 60s, serve as JSON on :11437

Keeps the ESP32 out of Yahoo Finance directly — the Mac handles the fetch
and the ESP32 polls http://192.168.0.51:11437/arm_stock instead.

GET /arm_stock  → {"price": 408.15, "change_pct": 15.53, "ts": 1780343768}
"""

import json
import threading
import time
import urllib.request
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

PORT      = 11437
SYMBOL    = "ARM"
POLL_S    = 60
LOG_FILE  = Path.home() / "Library" / "Logs" / "lango_arm_stock.log"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler(),
    ],
)
log = logging.getLogger("arm_stock")

_cache = {"price": 0.0, "change_pct": 0.0, "ts": 0, "error": "starting"}
_lock  = threading.Lock()


def _extract(body, key):
    needle = f'"{key}":'
    idx = body.find(needle)
    if idx < 0:
        return None
    idx += len(needle)
    token = body[idx : idx + 24].lstrip()
    try:
        return float(token.split(",")[0].split("}")[0].split("]")[0])
    except ValueError:
        return None


def fetch_once():
    url = (
        f"https://query1.finance.yahoo.com/v8/finance/chart/{SYMBOL}"
        "?interval=1d&range=1d"
    )
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read(4096).decode("utf-8", errors="replace")

        price = _extract(body, "regularMarketPrice")
        prev  = _extract(body, "chartPreviousClose")

        if price is None:
            log.warning("price field not found in response")
            return None

        change_pct = ((price - prev) / prev * 100.0) if prev else 0.0
        data = {
            "price":      round(price, 2),
            "change_pct": round(change_pct, 2),
            "ts":         int(time.time()),
        }
        with _lock:
            _cache.clear()
            _cache.update(data)
        log.info("ARM $%.2f  %+.2f%%", price, change_pct)
        return data

    except Exception as exc:
        with _lock:
            _cache["error"] = str(exc)
        log.error("fetch error: %s", exc)
        return None


class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/arm_stock", "/arm_stock.json", "/"):
            with _lock:
                body = json.dumps(_cache).encode()
            self.send_response(200)
            self.send_header("Content-Type",   "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control",  "no-store")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *_):  # suppress per-request access logs
        pass


def _fetch_loop():
    while True:
        time.sleep(POLL_S)
        fetch_once()


if __name__ == "__main__":
    log.info("Starting — initial fetch…")
    fetch_once()

    t = threading.Thread(target=_fetch_loop, daemon=True)
    t.start()

    log.info("HTTP server on 0.0.0.0:%d", PORT)
    HTTPServer(("0.0.0.0", PORT), _Handler).serve_forever()
