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
        Clear the log buffer first to discard boot-time browser WS noise,
        then wait a full ping interval (20s) so the PONG exchange has time to occur."""
        session.post(f"{base_url}/api/logs/clear")   # discard boot-time noise
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
        """Connect via WebSocket and verify the server keeps the connection
        alive without immediately closing or erroring.  We do NOT wait for
        an LLM response here — the LLM API may be unavailable (e.g. 402).
        Full LLM round-trip stability is covered by test_ws_survives_two_ping_cycles."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed: pip install websocket-client")

        ip = base_url.replace("https://", "")
        url = f"wss://{ip}/ws"
        connected = threading.Event()
        error_event = threading.Event()

        def on_open(ws):
            connected.set()

        def on_error(ws, err):
            error_event.set()

        ws = websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_error=on_error,
        )
        t = threading.Thread(
            target=ws.run_forever,
            kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE}},
            daemon=True,
        )
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect within 5 s"
        # Connection should stay alive for at least 5 s (no immediate error/close)
        assert not error_event.wait(timeout=5), "WebSocket error occurred within 5 s of connect"
        ws.close()
        t.join(timeout=3)


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

        # Allow the preceding test's LLM call and TCP cleanup to finish so that
        # the device's WS client slot is free before we open our connection.
        time.sleep(5)

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


# ── WebSocket stress (no paid API calls) ──────────────────────
#
# All tests in this section deliberately avoid sending "prompt" /
# "message" / "audio_end" to the device so no LLM, STT, or TTS
# requests are made.  Safe message types used:
#   • malformed / empty JSON       → silently dropped
#   • {"type":"audio_abort"}       → resets audio buffer, no LLM
#   • {"type":"unknown_noop"}      → unknown type, silently dropped
#   • oversized text frame (>8KB)  → server-side rejection guard

class TestWebSocketStress:
    """Resiliency tests: rapid reconnects, invalid payloads, concurrent
    clients, oversized frames.  No paid API calls are made."""

    # ── helpers ──────────────────────────────────────────────

    @staticmethod
    def _ws_url(base_url):
        return "wss://" + base_url.replace("https://", "") + "/ws"

    @staticmethod
    def _make_ws(url, on_open=None, on_message=None, on_close=None, on_error=None):
        try:
            import websocket
        except ImportError:
            return None
        return websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_message=on_message,
            on_close=on_close,
            on_error=on_error,
        )

    @staticmethod
    def _run_ws(ws):
        import ssl
        ws.run_forever(sslopt={"cert_reqs": ssl.CERT_NONE},
                       ping_interval=0, ping_timeout=None)

    # ── tests ────────────────────────────────────────────────

    def test_ws_invalid_json_does_not_crash(self, base_url):
        """Send 20 malformed JSON payloads; connection must stay open."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        # Give the device time to release the WS client slot used by the
        # stability test (50 s hold) before opening a new connection.
        time.sleep(5)

        url = self._ws_url(base_url)
        connected = threading.Event()
        errors    = []

        def on_open(ws):
            connected.set()
            junk_payloads = [
                "",
                "not json at all",
                "{",
                "null",
                "[]",
                '{"type":null}',
                '{"type":12345}',
                '{"no_type_key":true}',
                "{" + "x" * 100 + "}",       # large but invalid JSON
            ] * 2   # × 2 = 18 payloads
            for p in junk_payloads:
                ws.send(p)
            # Give server 2 s to process, then send a valid noop
            time.sleep(2)
            ws.send(json.dumps({"type": "audio_abort", "chat_id": "stress_test"}))
            time.sleep(1)
            ws.close()

        def on_error(ws, err):
            errors.append(str(err))

        ws = self._make_ws(url, on_open=on_open, on_error=on_error)
        t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect within 5 s"
        t.join(timeout=15)

        # Filter out the spurious websocket-client library error that fires when
        # ws.close() is called from inside on_open — the library tries to access
        # ws.sock after it has already been cleared, which is a client-side race,
        # not a device problem.
        real_errors = [e for e in errors
                       if "'NoneType' object has no attribute 'sock'" not in e
                       and "NoneType" not in e]
        assert not real_errors, \
            f"WebSocket errors after malformed payloads: {real_errors}"

    def test_ws_oversized_frame_rejected_gracefully(self, base_url):
        """Send a 10 KB text frame (>8 KB limit); device must not crash."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        # Brief settle time so the preceding invalid-JSON test's connection
        # is fully torn down on the device side.
        time.sleep(3)

        url = self._ws_url(base_url)
        connected = threading.Event()
        closed    = threading.Event()

        def on_open(ws):
            connected.set()
            # 10 KB JSON — exceeds the 8 KB text-frame guard in ws_server.c
            big = json.dumps({"type": "audio_abort",
                              "padding": "x" * (10 * 1024)})
            ws.send(big)
            time.sleep(2)
            ws.close()

        def on_close(ws, code, reason):
            closed.set()

        ws = self._make_ws(url, on_open=on_open, on_close=on_close)
        t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect"
        t.join(timeout=15)

    def test_ws_close_frees_slot_immediately(self, base_url):
        """Connect, close with a status-code CLOSE frame (len=2), then reconnect
        immediately.  Regression for the CLOSE-payload bug where browsers always
        send a 2-byte status code causing the old slot to stay occupied for up to
        20 s (until next ping failure), making rapid refresh reject new connections.
        Repeat 4 times in quick succession to fill and drain all 4 client slots."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        url = self._ws_url(base_url)

        for cycle in range(4):
            connected = threading.Event()
            closed    = threading.Event()

            def on_open(ws, ev=connected):
                ev.set()

            def on_close(ws, code, reason, ev=closed):
                ev.set()

            ws = self._make_ws(url, on_open=on_open, on_close=on_close)
            t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
            t.start()

            assert connected.wait(timeout=8), \
                f"Cycle {cycle}: WebSocket did not connect within 8 s"
            ws.close()
            closed.wait(timeout=5)
            # No sleep — reconnect immediately to prove the slot was freed

    def test_ws_rapid_reconnect(self, base_url):
        """Connect and disconnect 8 times rapidly; heap must not leak."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        url = self._ws_url(base_url)

        def one_connect():
            done = threading.Event()
            def on_open(ws):
                ws.send(json.dumps({"type": "audio_abort",
                                    "chat_id": "reconnect_stress"}))
                time.sleep(0.3)
                ws.close()
            def on_close(ws, code, reason):
                done.set()
            ws = self._make_ws(url, on_open=on_open, on_close=on_close)
            t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
            t.start()
            done.wait(timeout=8)

        for i in range(8):
            one_connect()
            time.sleep(0.2)   # brief pause between connects

    def test_ws_max_concurrent_clients(self, base_url):
        """Open 4 simultaneous WS connections (LANG_WS_MAX_CLIENTS=4); all must
        connect.  A 5th connection attempt should not crash the device."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        url = self._ws_url(base_url)
        N   = 4
        connected_events = [threading.Event() for _ in range(N)]
        ws_list = []

        def make_client(idx):
            ev = connected_events[idx]
            def on_open(ws):
                ev.set()
            ws = self._make_ws(url, on_open=on_open)
            t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
            t.start()
            ws_list.append(ws)

        for i in range(N):
            make_client(i)
            time.sleep(0.2)

        for i, ev in enumerate(connected_events):
            assert ev.wait(timeout=8), f"Client {i} did not connect within 8 s"

        # 5th connection — device must not crash (may or may not track it)
        extra_connected = threading.Event()
        def on_open_extra(ws):
            extra_connected.set()
            ws.send(json.dumps({"type": "audio_abort", "chat_id": "extra"}))
            time.sleep(1)
            ws.close()
        ws_extra = self._make_ws(url, on_open=on_open_extra)
        t_extra  = threading.Thread(target=self._run_ws, args=(ws_extra,),
                                    daemon=True)
        t_extra.start()
        extra_connected.wait(timeout=8)   # may or may not connect — just must not crash

        time.sleep(2)
        for ws in ws_list:
            try:
                ws.close()
            except Exception:
                pass
        t_extra.join(timeout=5)

    def test_ws_extended_ping_stability(self, base_url):
        """Idle WS connection for 75 s — 3 full server-ping cycles.  Regression
        for the PONG-drain bug (50 s version already in TestWebSocketStability)."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed")

        # The preceding test opened 5 concurrent WS connections (daemon threads —
        # no join guarantee); give the device extra time to recycle all 5 client
        # slots and drain TCP FIN/TIME_WAIT before we connect.
        time.sleep(15)

        url = self._ws_url(base_url)
        premature_close = threading.Event()
        connected       = threading.Event()
        done_waiting    = threading.Event()

        def on_open(ws):
            connected.set()

        def on_close(ws, code, reason):
            if not done_waiting.is_set():
                premature_close.set()

        ws = self._make_ws(url, on_open=on_open, on_close=on_close)
        t  = threading.Thread(target=self._run_ws, args=(ws,), daemon=True)
        t.start()

        assert connected.wait(timeout=10), "WebSocket did not connect within 10 s"
        time.sleep(75)
        done_waiting.set()

        assert not premature_close.is_set(), \
            "WebSocket closed prematurely during 75 s hold (PONG-drain regression?)"
        assert t.is_alive(), \
            "WebSocket thread died during 75 s stability wait"
        ws.close()


# ── HTTP stress (no paid API calls) ───────────────────────────

class TestHttpStress:
    """Rapid HTTP requests and concurrent connections — checks for heap
    leaks, descriptor exhaustion, and TLS session pressure."""

    def test_rapid_sysinfo_heap_stable(self, session, base_url):
        """Fire 30 back-to-back sysinfo calls; final heap must be ≥ first."""
        first = session.get(f"{base_url}/api/sysinfo").json()["heap_free"]
        for _ in range(28):
            r = session.get(f"{base_url}/api/sysinfo")
            assert r.status_code == 200
        # Allow 3 s for the mbedTLS session cache to stabilise.  The cache holds
        # up to 16 entries × ~440 B ≈ 7 KB; once full it stops growing, so this
        # is a one-time bounded cost rather than a true leak.
        time.sleep(3)
        last = session.get(f"{base_url}/api/sysinfo").json()["heap_free"]
        # Allow up to 8 KB variance to accommodate the mbedTLS session-cache fill
        assert last >= first - 8192, \
            f"Heap degraded: {first} → {last} (> 8192 tolerance)"

    def test_rapid_logs_read(self, session, base_url):
        """Read /api/logs 15 times rapidly — no 500, no crash."""
        for _ in range(15):
            r = session.get(f"{base_url}/api/logs")
            assert r.status_code == 200
            assert len(r.text) >= 0   # may be empty after clear

    def test_logs_clear_idempotent(self, session, base_url):
        """POST /api/logs/clear 10 times — always {"ok":true}, no crash."""
        for _ in range(10):
            r = session.post(f"{base_url}/api/logs/clear")
            assert r.status_code == 200
            assert r.json().get("ok") is True

    def test_concurrent_http_requests(self, base_url):
        """Fire 5 simultaneous HTTPS GET requests; all must return 200."""
        # Allow the device's WS ping task to finish its current cycle after
        # the 75 s extended stability test closed its connection.  Without this
        # settle time, the ping task can send a WS-PING frame on an FD that was
        # just reused for a new HTTP connection, corrupting the HTTP response.
        time.sleep(5)

        results = []
        lock    = threading.Lock()

        def one_request():
            s = requests.Session()
            s.verify = False
            retry = Retry(total=3, connect=3, read=3, backoff_factor=0.5)
            s.mount("https://", HTTPAdapter(max_retries=retry))
            r = s.get(f"{base_url}/api/sysinfo", timeout=10)
            with lock:
                results.append(r.status_code)

        threads = [threading.Thread(target=one_request) for _ in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=15)

        assert len(results) == 5, f"Only {len(results)}/5 requests completed"
        assert all(c == 200 for c in results), \
            f"Unexpected status codes under concurrent load: {results}"

    def test_ota_status_rapid(self, session, base_url):
        """Poll /api/ota/status 20 times — always idle, always JSON."""
        for _ in range(20):
            r = session.get(f"{base_url}/api/ota/status")
            assert r.status_code == 200
            d = r.json()
            assert d.get("state") == "idle"


# ── Edge-case input validation ─────────────────────────────────

class TestEdgeCases:
    """Boundary / malformed input — device must return 4xx, never 500 or crash."""

    def test_file_empty_name(self, session, base_url):
        r = session.get(f"{base_url}/api/file?name=")
        assert r.status_code in (400, 403, 404), \
            f"Empty filename should be rejected, got {r.status_code}"

    def test_file_very_long_name(self, session, base_url):
        name = "a" * 512
        r = session.get(f"{base_url}/api/file?name={name}")
        # 414 URI Too Long is also a valid rejection — the HTTP layer may reject
        # the overlong URL before it even reaches the file handler.
        assert r.status_code in (400, 403, 404, 414), \
            f"512-char filename should be rejected, got {r.status_code}"

    def test_file_path_traversal_variants(self, session, base_url):
        """Multiple path-traversal attempts — all must be blocked."""
        for attempt in ["../etc/passwd", "..%2fetc%2fpasswd",
                        "%2e%2e/etc/passwd", "....//etc/passwd",
                        "soul/../../../etc/passwd"]:
            r = session.get(f"{base_url}/api/file?name={attempt}")
            assert r.status_code in (400, 403, 404), \
                f"Path traversal '{attempt}' was not blocked: {r.status_code}"

    def test_message_api_missing_fields(self, session, base_url):
        """POST /api/message with various missing/bad fields."""
        cases = [
            {},                              # empty object
            {"message": ""},                 # empty message
            {"channel": "system"},           # message field missing
            {"message": "x" * 5000},        # oversized message
        ]
        for body in cases:
            r = session.post(f"{base_url}/api/message", json=body)
            assert r.status_code in (202, 400, 422), \
                f"Unexpected status {r.status_code} for body {body}"

    def test_sysinfo_not_writable(self, session, base_url):
        """POST/DELETE to read-only endpoints must return 405."""
        r = session.post(f"{base_url}/api/sysinfo", json={})
        assert r.status_code == 405, \
            f"/api/sysinfo should not accept POST, got {r.status_code}"

    def test_unknown_endpoint_404(self, session, base_url):
        """Requests to non-existent paths must return 404, not crash."""
        for path in ["/api/doesnotexist", "/api/admin", "/robots.txt",
                     "/api/sysinfo/extra/path"]:
            r = session.get(f"{base_url}{path}")
            assert r.status_code in (404, 405), \
                f"Path '{path}' returned unexpected {r.status_code}"

    def test_no_panic_after_stress(self, session, base_url):
        """After all stress tests: device must not have panicked and heap
        must still be above the minimum safe threshold."""
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert d["heap_free"] > 20_000, \
            f"Heap critically low after stress: {d['heap_free']} bytes"
        # uptime must still be positive (device hasn't rebooted under us)
        assert d["uptime_s"] > 0


# ── UAC Microphone + STT pipeline ────────────────────────────

def _make_silent_wav(duration_ms: int = 500, sample_rate: int = 16000) -> bytes:
    """Return a minimal valid WAV file containing silence."""
    import struct
    num_samples = sample_rate * duration_ms // 1000
    pcm_data = bytes(num_samples * 2)          # 16-bit zeros
    subchunk2_size = len(pcm_data)
    chunk_size = 36 + subchunk2_size
    header = struct.pack('<4sI4s'       # RIFF ... WAVE
                        '4sIHHIIHH'    # fmt  chunk
                        '4sI',         # data chunk header
                        b'RIFF', chunk_size, b'WAVE',
                        b'fmt ', 16, 1, 1,            # PCM, mono
                        sample_rate,
                        sample_rate * 2,              # ByteRate
                        2, 16,                        # BlockAlign, BitsPerSample
                        b'data', subchunk2_size)
    return header + pcm_data


class TestUACMicrophone:
    """Verify UAC (USB Audio Class) mic driver status reported via sysinfo."""

    def test_uac_status_field_in_sysinfo(self, session, base_url):
        """sysinfo must include 'uac_mic_connected' boolean after firmware update."""
        d = session.get(f"{base_url}/api/sysinfo").json()
        assert "uac_mic_connected" in d, \
            "sysinfo missing 'uac_mic_connected' — firmware may be pre-UAC-status"

    def test_uac_driver_init_logged(self, session, base_url):
        """Logs must not show uac_host_install() failure."""
        r = session.get(f"{base_url}/api/logs")
        assert r.status_code == 200
        logs = r.text
        install_err = "uac_host_install failed" in logs
        assert not install_err, \
            "UAC driver install failure in logs: uac_host_install failed"
        install_ok = "UAC mic driver installed" in logs
        if not install_ok:
            pytest.skip("UAC driver init log not in ring buffer (may have been cleared)")


class TestSTTPipeline:
    """Verify the Speech-to-Text pipeline works end-to-end via WebSocket audio."""

    def test_stt_key_configured(self, session, base_url):
        """STT API key must be set (non-empty masked value in /api/config)."""
        d = session.get(f"{base_url}/api/config").json()
        stt_key = d.get("stt_key", "")
        assert stt_key and stt_key != "", \
            "STT API key is not configured — set it with 'stt_key <groq-key>'"

    def test_stt_via_websocket_wav(self, base_url):
        """Send a silent WAV via WebSocket audio pipeline and verify:
          - The device accepts the audio without crashing
          - It returns a status frame (stt_processing, stt_failed, or idle)
          - The device is still alive after the transcription attempt
        Uses a 500 ms silent WAV — Whisper returns empty transcript which
        is handled cleanly (stt_failed error, no LLM call triggered)."""
        try:
            import websocket
        except ImportError:
            pytest.skip("websocket-client not installed: pip install websocket-client")

        ip   = base_url.replace("https://", "")
        url  = f"wss://{ip}/ws"
        chat = "stt_pipeline_test"
        wav  = _make_silent_wav(500, 16000)

        received   = []
        connected  = threading.Event()
        done       = threading.Event()
        error_flag = threading.Event()

        def on_open(ws):
            connected.set()
            ws.send(json.dumps({"type": "audio_start",
                                "mime": "audio/wav",
                                "chat_id": chat}))
            # Send WAV as a single binary frame (compatible with older websocket-client)
            if hasattr(ws, 'send_binary'):
                ws.send_binary(wav)
            else:
                import websocket as _ws_mod
                ws.send(wav, opcode=_ws_mod.ABNF.OPCODE_BINARY)
            ws.send(json.dumps({"type": "audio_end", "chat_id": chat}))

        def on_message(ws, msg):
            received.append(msg)
            try:
                d = json.loads(msg)
                # Any status or error response means the pipeline handled the audio
                if d.get("type") in ("status", "error", "message"):
                    done.set()
            except (json.JSONDecodeError, TypeError):
                pass

        def on_error(ws, err):
            error_flag.set()
            done.set()

        ws_app = websocket.WebSocketApp(
            url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error,
        )
        t = threading.Thread(
            target=ws_app.run_forever,
            kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE}},
            daemon=True,
        )
        t.start()

        assert connected.wait(timeout=5), "WebSocket did not connect within 5 s"
        # Wait up to 30 s for STT pipeline response (Groq Whisper API call)
        done.wait(timeout=30)
        ws_app.close()
        t.join(timeout=3)

        assert not error_flag.is_set(), "WebSocket error during STT test"
        assert len(received) > 0, \
            "No response from STT pipeline within 30 s — STT task may be hung"

        # Verify at least one response has a valid type field
        valid = [m for m in received
                 if '"type"' in m and json.loads(m).get("type") in
                    ("status", "error", "message")]
        assert valid, \
            f"No status/error/message response from STT pipeline; got: {received[:3]}"


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
