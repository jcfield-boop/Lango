#!/usr/bin/env python3
"""
test_device.py — Integration tests for Langoustine over HTTPS/WebSocket.

Usage:
    pip install pytest requests websocket-client
    pytest scripts/test_device.py -v                     # uses default IP
    pytest scripts/test_device.py -v --ip 192.168.0.44

Tests run against the live device.  The device must be reachable and
fully booted.  No serial cable needed.

Exit 0 = all tests pass.  Exit != 0 = something is wrong on the device.
"""

import json
import time
import threading
import ssl
import pytest
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# ── Configuration ──────────────────────────────────────────────

def pytest_addoption(parser):
    parser.addoption("--ip", default="192.168.0.44",
                     help="Device IP address (default: 192.168.0.44)")

@pytest.fixture(scope="session")
def base_url(request):
    ip = request.config.getoption("--ip")
    return f"https://{ip}"

@pytest.fixture(scope="session")
def session(base_url):
    s = requests.Session()
    s.verify = False          # self-signed cert
    s.timeout = 10
    # Disable persistent keep-alive connections so stale TCP connections
    # (e.g. after the 50 s WebSocket stability test) are never reused.
    s.headers.update({"Connection": "close"})
    # Retry up to 3 times on transient connection errors (RemoteDisconnected,
    # ProtocolError, etc.) with a 1 s backoff.  This is the final safety net
    # for the test that runs immediately after the 50 s WS stability hold.
    retry = Retry(total=3, connect=3, read=3, redirect=False, backoff_factor=1)
    s.mount("https://", HTTPAdapter(max_retries=retry))
    return s

# Suppress the InsecureRequestWarning for self-signed cert
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


# ── /api/sysinfo ──────────────────────────────────────────────

class TestSysinfo:
    def test_returns_200(self, session, base_url):
        r = session.get(f"{base_url}/api/sysinfo")
        assert r.status_code == 200

    def test_json_has_required_keys(self, session, base_url):
        d = session.get(f"{base_url}/api/sysinfo").json()
        required = ["heap_free", "heap_min", "psram_free",
                    "lfs_total", "lfs_used", "uptime_s", "reset_reason"]
        for k in required:
            assert k in d, f"Missing key: {k}"

    def test_heap_above_minimum(self, session, base_url):
        """SRAM guard: panic below ~20 KB historically; warn below 25 KB."""
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert d["heap_free"] > 25_000, \
            f"Dangerously low SRAM: {d['heap_free']} bytes free"

    def test_heap_min_above_minimum(self, session, base_url):
        """heap_min tracks worst-case allocation; should stay > 20 KB."""
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert d["heap_min"] > 20_000, \
            f"heap_min too low: {d['heap_min']} — prior peak allocation was too large"

    def test_psram_above_minimum(self, session, base_url):
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert d["psram_free"] > 10_000_000, \
            f"PSRAM low: {d['psram_free']} bytes"

    def test_lfs_has_space(self, session, base_url):
        d = session.get(f"{base_url}/api/sysinfo").json()
        used_pct = d["lfs_used"] / d["lfs_total"] * 100
        assert used_pct < 90, f"LittleFS {used_pct:.1f}% full"

    def test_no_panic_since_boot(self, session, base_url):
        """Device should not have reset_reason=panic on the current boot.
        A panic indicates the firmware is unstable."""
        d = session.get(f"{base_url}/api/sysinfo").json()
        # Allow panic only if device uptime is very long (prior boot was bad)
        # If uptime < 60s and reason==panic, firmware crashed on this boot.
        if d["uptime_s"] < 60 and d["reset_reason"] == "panic":
            pytest.fail(
                f"Device panicked during current boot "
                f"(uptime={d['uptime_s']}s, reason={d['reset_reason']})"
            )

    def test_uptime_positive(self, session, base_url):
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert d["uptime_s"] > 0


# ── /api/file ─────────────────────────────────────────────────

class TestFileApi:
    def test_soul_readable(self, session, base_url):
        r = session.get(f"{base_url}/api/file?name=soul")
        assert r.status_code == 200
        assert len(r.text) > 10, "SOUL.md is empty"

    def test_user_readable(self, session, base_url):
        r = session.get(f"{base_url}/api/file?name=user")
        assert r.status_code == 200

    def test_memory_readable(self, session, base_url):
        r = session.get(f"{base_url}/api/file?name=memory")
        assert r.status_code in (200, 404)  # may not exist yet

    def test_unknown_name_rejected(self, session, base_url):
        r = session.get(f"{base_url}/api/file?name=../../etc/passwd")
        assert r.status_code in (400, 403, 404)


# ── /api/logs ─────────────────────────────────────────────────

class TestLogsApi:
    def test_returns_200(self, session, base_url):
        r = session.get(f"{base_url}/api/logs")
        assert r.status_code == 200

    def test_not_flooded_with_periodic_noise(self, session, base_url):
        """After the log-verbosity fix, periodic heartbeats are LOGD.
        The ring buffer should contain real events, not just idle spam."""
        r = session.get(f"{base_url}/api/logs")
        log_text = r.text
        uac_idle_count = log_text.count("UAC PTT idle")
        uvc_alive_count = log_text.count("usb_lib_task alive")
        # With LOGD, these should NOT appear in a normal INFO-level log buffer
        assert uac_idle_count == 0, \
            f"UAC idle log still at INFO level: {uac_idle_count} occurrences"
        assert uvc_alive_count == 0, \
            f"usb_lib_task alive still at INFO level: {uvc_alive_count} occurrences"

    def test_no_invalid_json_warnings(self, session, base_url):
        """After the PONG-frame fix, 'Invalid JSON' should not appear.
        Wait a full ping interval (20s) so the PONG exchange has time to occur."""
        time.sleep(22)
        r = session.get(f"{base_url}/api/logs")
        assert "Invalid JSON" not in r.text, \
            "Spurious 'Invalid JSON' warning still appearing (PONG frame fix may not be active)"


# ── /api/message (async agent trigger) ───────────────────────

class TestMessageApi:
    def test_queues_message(self, session, base_url):
        payload = {"message": "test ping from pytest", "channel": "system", "chat_id": "pytest"}
        r = session.post(f"{base_url}/api/message", json=payload)
        assert r.status_code == 202
        d = r.json()
        assert d.get("status") == "queued"

    def test_rejects_empty_body(self, session, base_url):
        r = session.post(f"{base_url}/api/message",
                         data="", headers={"Content-Type": "application/json"})
        assert r.status_code in (400, 422)


# ── /api/ota/status ──────────────────────────────────────────

class TestOtaStatus:
    def test_idle_when_not_updating(self, session, base_url):
        r = session.get(f"{base_url}/api/ota/status")
        assert r.status_code == 200
        d = r.json()
        assert d.get("state") == "idle", \
            f"OTA state is not idle: {d.get('state')}"


# ── WebSocket connectivity ────────────────────────────────────

class TestWebSocket:
    def test_ws_connects_and_responds(self, base_url):
        """Connect via WebSocket and verify the server doesn't immediately
        close the connection or send an error on connect."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed: pip install websocket-client")

        ip = base_url.replace("https://", "")
        url = f"wss://{ip}/ws"
        received = []
        connected = threading.Event()
        done = threading.Event()

        def on_open(ws):
            connected.set()
            # Send a valid prompt — agent may or may not respond quickly
            ws.send(json.dumps({"type": "prompt", "content": "ping"}))

        def on_message(ws, msg):
            received.append(msg)
            done.set()
            ws.close()

        def on_error(ws, err):
            done.set()

        ws = websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error,
        )
        t = threading.Thread(
            target=ws.run_forever,
            kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE}},
            daemon=True,
        )
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect within 5 s"
        # Give agent up to 30 s to respond (it may need to query LLM)
        done.wait(timeout=30)

        assert len(received) > 0, "No WebSocket response received from device"
        # Response must be valid JSON
        msg = json.loads(received[0])
        assert "type" in msg, f"Response missing 'type': {received[0][:200]}"


# ── WebSocket ping stability (regression: PONG payload drain) ─

class TestWebSocketStability:
    """Verify the WebSocket connection survives multiple server→client ping
    cycles.  The PONG-payload-drain bug caused a disconnect after ~40-60 s
    (1-2 ping cycles × 20 s each) because unread payload bytes in the TCP
    receive buffer corrupted the next frame header parse."""

    def test_ws_survives_two_ping_cycles(self, base_url):
        """Keep an idle WS connection open for 50 s (≥ 2 × 20 s server-ping interval)
        and verify the connection is still alive.  Regression for the PONG-payload-drain
        bug where unread payload bytes corrupted the next frame header causing a
        disconnect after ~40-60 s."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed: pip install websocket-client")

        ip = base_url.replace("https://", "")
        url = f"wss://{ip}/ws"
        premature_close = threading.Event()
        connected = threading.Event()

        def on_open(ws):
            connected.set()
            # Do NOT send a prompt — we just hold the connection idle so only
            # PING→PONG control-frame traffic occurs. Sending agent prompts
            # adds noise and can trigger reboots under test load.

        def on_close(ws, code, reason):
            # on_close fires when connection drops unexpectedly OR when we call
            # ws.close() at the end of the test.  We distinguish by whether
            # the 50 s wait has completed yet (signalled via connected being set
            # with the thread still running).
            if not done_waiting.is_set():
                premature_close.set()

        ws = websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_close=on_close,
        )
        done_waiting = threading.Event()
        t = threading.Thread(
            target=ws.run_forever,
            kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE},
                    "ping_interval": 0,   # disable client-side ping; let server lead
                    "ping_timeout": None},
            daemon=True,
        )
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect within 5 s"

        # Hold the connection idle across two full server-ping cycles (2 × 20 s).
        # The server sends PING every 20 s; the browser responds with PONG.
        # Before the payload-drain fix, the unread PONG payload corrupted the
        # next frame parse, closing the connection after ~40-60 s.
        time.sleep(50)
        done_waiting.set()

        assert not premature_close.is_set(), \
            "WebSocket closed prematurely during 50 s stability wait (PONG-drain bug?)"
        assert t.is_alive(), \
            "WebSocket thread died during 50 s stability wait (connection dropped)"

        ws.close()


# ── Health summary ────────────────────────────────────────────

class TestHealthSummary:
    def test_crash_log_exists(self, session, base_url):
        """The crash log endpoint should be accessible (empty is fine)."""
        r = session.get(f"{base_url}/api/file?name=memory")
        # Just check it doesn't return a 500
        assert r.status_code != 500


if __name__ == "__main__":
    import sys
    sys.exit(pytest.main([__file__, "-v"] + sys.argv[1:]))
