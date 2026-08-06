import os
import tempfile
import unittest

from delivery import CANARY_INTERVAL_SECONDS, SILENCEABLE, canary_due
from heartbeat import Heartbeat, should_ping
from store import Store


class TestShouldPing(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path, clock=lambda: 0.0)

    def tearDown(self):
        os.unlink(self.path)

    def test_pings_when_everything_is_healthy(self):
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_failed_cycle_suppresses_ping(self):
        self.assertFalse(should_ping(self.store, cycle_committed=False,
                                     worker_alive=True, now=1.0, deadline=300.0))

    def test_dead_delivery_worker_suppresses_ping(self):
        """The failure this gating exists for: collection fine, alerting dead."""
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=False, now=1.0, deadline=300.0))

    def test_overdue_emergency_item_suppresses_ping(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=1e6, deadline=300.0))

    def test_unreachable_nodes_do_not_suppress_ping(self):
        """The heartbeat means the watcher works, not that the fleet is healthy."""
        self.store.open_incident("majority_unreachable", ("a", "b"), "normal", "down")
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_an_emergency_within_its_deadline_does_not_suppress_ping(self):
        """Guards the gate-as-constant regression already fixed once in the
        store: if `deadline` were hardcoded instead of forwarded, an emergency
        still inside its window would wrongly suppress the ping."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_a_non_numeric_now_fails_closed(self):
        """A bad clock read (None) binds as SQL NULL, which every comparison
        against it reads as "nothing overdue" -- silent fail-open. Seed an
        incident that IS overdue under a real clock so the test demonstrates
        the actual danger (a genuinely overdue critical goes unnoticed),
        not just input-validation hygiene."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertTrue(self.store.has_overdue_critical(now=1000.0, deadline=300.0))
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=None, deadline=300.0))


class TestHeartbeatPing(unittest.TestCase):
    """Heartbeat had zero tests. These pin the transport contract: success on
    2xx, no propagation on transport failure, and no body on the wire."""

    class _Response:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def _with_urlopen(self, fake):
        import heartbeat
        original = heartbeat.urllib.request.urlopen
        heartbeat.urllib.request.urlopen = fake
        self.addCleanup(setattr, heartbeat.urllib.request, "urlopen", original)

    def test_a_2xx_response_succeeds(self):
        self._with_urlopen(lambda request, timeout=None: self._Response())
        self.assertTrue(Heartbeat("https://example.invalid/ping").ping())

    def test_transport_exceptions_return_false_rather_than_propagating(self):
        """urllib only wraps errors from h.request(); getresponse() and read()
        propagate raw. ConnectionResetError and http.client.RemoteDisconnected
        (the routine keep-alive-proxy blip) are exactly what took the watcher
        down when this only caught (URLError, TimeoutError)."""
        import http.client
        import ssl

        for exc in (ConnectionResetError("reset"),
                    http.client.RemoteDisconnected("disconnected"),
                    http.client.IncompleteRead(b""),
                    ssl.SSLError("handshake"),
                    OSError("unreachable")):
            def boom(request, timeout=None, _e=exc):
                raise _e

            self._with_urlopen(boom)
            self.assertFalse(Heartbeat("https://example.invalid/ping").ping(),
                             f"{type(exc).__name__} must not propagate")

    def test_a_malformed_url_returns_false_rather_than_raising(self):
        """urlopen() itself raises ValueError for an unrecognized URL scheme
        (e.g. a misconfigured heartbeat URL) -- this is why ValueError is in
        the catch tuple, not because of JSON parsing (ping() does none)."""
        self.assertFalse(Heartbeat("not-a-valid-url").ping())

    def test_the_request_carries_no_body(self):
        """Liveness signal only: no node details, no secrets on the wire."""
        captured = {}

        def fake(request, timeout=None):
            captured["data"] = request.data
            captured["method"] = request.get_method()
            return self._Response()

        self._with_urlopen(fake)
        Heartbeat("https://example.invalid/ping").ping()
        self.assertIsNone(captured["data"])
        self.assertEqual(captured["method"], "GET")


class TestCanary(unittest.TestCase):
    """The overdue gate is reactive -- it can only test a path that already
    had traffic. The canary exercises the alert path on a schedule so a bad
    credential is discovered before an emergency needs it."""

    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path, clock=lambda: 0.0)

    def tearDown(self):
        os.unlink(self.path)

    def test_last_canary_at_is_none_before_any_enqueue(self):
        self.assertIsNone(self.store.last_canary_at())

    def test_last_canary_at_reflects_the_frozen_clock(self):
        self.store.enqueue_canary()
        self.assertEqual(self.store.last_canary_at(), 0.0)

    def test_canary_due_when_never_enqueued(self):
        self.assertTrue(canary_due(self.store, now=0.0))

    def test_canary_not_due_immediately_after_enqueue(self):
        self.store.enqueue_canary()
        self.assertFalse(canary_due(self.store, now=0.0))

    def test_canary_due_again_after_the_interval_elapses(self):
        self.store.enqueue_canary()
        self.assertTrue(canary_due(self.store, now=CANARY_INTERVAL_SECONDS))

    def test_canary_is_silenceable_during_maintenance(self):
        self.assertIn("canary", SILENCEABLE)


if __name__ == "__main__":
    unittest.main()
