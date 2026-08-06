# tools/fleet-watcher/tests/test_notify.py
import os
import tempfile
import unittest

from delivery import drain
from store import Store


class FakeNotifier:
    def __init__(self, fail_times=0):
        self.sent = []
        self.fail_times = fail_times

    def send(self, title, message, priority):
        if self.fail_times > 0:
            self.fail_times -= 1
            return False
        self.sent.append((title, message, priority))
        return True


class TestDelivery(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        # clock=lambda: 0.0 so the store's own timestamps (used by mark_failed
        # / defer) and the synthetic `now` values passed to drain/pending_outbox
        # share one time domain. A real clock (>1e9 since 2001) would push
        # next_attempt_at past any of the synthetic `now` values used below.
        self.store = Store(self.path, clock=lambda: 0.0)

    def tearDown(self):
        os.unlink(self.path)

    def test_pending_item_is_delivered_once(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        drain(self.store, n, now=2.0)
        self.assertEqual(len(n.sent), 1)
        self.assertEqual(n.sent[0][2], "emergency")

    def test_failed_delivery_is_retried_later_not_lost(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier(fail_times=1)
        drain(self.store, n, now=1.0)
        self.assertEqual(n.sent, [])
        self.assertEqual(len(self.store.pending_outbox(now=1e9)), 1)
        drain(self.store, n, now=1e9)
        self.assertEqual(len(n.sent), 1)

    def test_recovery_notification_is_sent_on_close(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        self.store.close_incident(iid)
        drain(self.store, n, now=2.0)
        self.assertEqual(len(n.sent), 2)
        self.assertIn("resolved", n.sent[1][0])

    def test_maintenance_suppresses_silenceable_rules_only(self):
        self.store.open_incident("node_behind", ("c",), "normal", "12 blocks")
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        rules = [t for t, _, _ in n.sent]
        self.assertTrue(any("safe_mode" in r for r in rules))
        self.assertFalse(any("node_behind" in r for r in rules))

    def test_title_text_cannot_affect_silencing(self):
        """Silencing reads the stored rule, never the human-readable title."""
        self.store.open_incident("node_behind", ("c",), "normal", "12 blocks")
        self.store.conn.execute("UPDATE outbox SET title='completely unrelated'")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(n.sent, [], "still silenced despite the retitle")
        drain(self.store, FakeNotifier(), now=2.0, maintenance=False)

    def test_recovery_item_is_silenceable_after_its_incident_closed(self):
        """The outbox row must stand alone once the incident is gone."""
        iid = self.store.open_incident("node_behind", ("c",), "normal", "12")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(n.sent, [])

    def test_silent_rule_is_recorded_but_never_delivered(self):
        self.store.open_incident("node_restart", ("a",), "normal", "restarted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        self.assertEqual(n.sent, [])
        self.assertEqual(self.store.pending_outbox(now=1e9), [],
                         "consumed, not left retrying forever")

    def test_maintenance_never_suppresses_observer_divergence(self):
        self.store.open_incident("observer_divergence", ("w",), "normal", "forked")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(len(n.sent), 1)

    def test_silenced_item_is_deferred_not_consumed(self):
        """A silenced item must remain deliverable once the window ends, and
        once delivered its title still names the rule. Consuming it via
        mark_sent would mean the operator's only message for the whole
        episode is the eventual "resolved", for a condition they were never
        told about."""
        self.store.open_incident("node_behind", ("c",), "normal", "12 blocks")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(n.sent, [])
        # Not consumed: still present and undelivered, attempts untouched —
        # nothing went wrong, so this isn't a failure either.
        pending = self.store.pending_outbox(now=1e9)
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0].attempts, 0)
        # Once maintenance ends (or the window is re-evaluated), it delivers.
        drain(self.store, n, now=1e9, maintenance=False)
        self.assertEqual(len(n.sent), 1)
        self.assertIn("node_behind", n.sent[0][0])

    def test_a_raising_transport_does_not_block_later_items(self):
        """A transport exception on one item must not abort the whole drain
        cycle. A later, unrelated item (e.g. tip_divergence queued behind a
        raising safe_mode send) still gets its chance, and the raising item
        backs off instead of retrying immediately with attempts=0."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.store.open_incident("tip_divergence", ("b",), "emergency", "forked")

        class RaisingThenOkNotifier:
            def __init__(self):
                self.sent = []

            def send(self, title, message, priority):
                if "safe_mode" in title:
                    raise ConnectionResetError("boom")
                self.sent.append((title, message, priority))
                return True

        n = RaisingThenOkNotifier()
        delivered = drain(self.store, n, now=1.0)
        self.assertEqual(delivered, 1)
        self.assertTrue(any("tip_divergence" in t for t, _, _ in n.sent),
                        "later item must not be head-of-line-blocked")
        # The raising item must have backed off, not stayed at
        # attempts=0/next_attempt_at=0.0 (which would retry with no delay).
        self.assertEqual(self.store.pending_outbox(now=1.0), [],
                         "raising item must not be immediately retryable")
        self.assertEqual(len(self.store.pending_outbox(now=1e9)), 1)


class TestPushoverNotifier(unittest.TestCase):
    """notify.py had no coverage at all — half the deliverable. These pin the
    two things that decide whether a page actually arrives."""

    def _notifier(self):
        from notify import PushoverNotifier
        return PushoverNotifier("tok", "usr")

    class _Response:
        status = 200

        def __init__(self, body):
            self._body = body

        def read(self):
            return self._body

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def _with_urlopen(self, fake):
        import notify
        original = notify.urllib.request.urlopen
        notify.urllib.request.urlopen = fake
        self.addCleanup(setattr, notify.urllib.request, "urlopen", original)

    def test_emergency_sets_priority_retry_and_a_three_hour_expire(self):
        captured = {}

        def fake(request, timeout=None):
            captured["body"] = request.data.decode()
            return self._Response(b'{"status":1}')

        self._with_urlopen(fake)
        self.assertTrue(self._notifier().send("t", "m", "emergency"))
        self.assertIn("priority=2", captured["body"])
        self.assertIn("expire=10800", captured["body"])
        self.assertIn("retry=", captured["body"])

    def test_normal_priority_sets_no_retry_parameters(self):
        captured = {}

        def fake(request, timeout=None):
            captured["body"] = request.data.decode()
            return self._Response(b'{"status":1}')

        self._with_urlopen(fake)
        self._notifier().send("t", "m", "normal")
        self.assertNotIn("priority=2", captured["body"])
        self.assertNotIn("expire=", captured["body"])

    def test_transport_exceptions_return_false_rather_than_propagating(self):
        import http.client
        import ssl

        for exc in (ConnectionResetError("reset"),
                    http.client.IncompleteRead(b""),
                    ssl.SSLError("handshake"),
                    OSError("unreachable"),
                    ValueError("not json")):
            def boom(request, timeout=None, _e=exc):
                raise _e

            self._with_urlopen(boom)
            self.assertFalse(self._notifier().send("t", "m", "normal"),
                             f"{type(exc).__name__} must not propagate")

    def test_a_rejected_message_is_not_reported_as_sent(self):
        self._with_urlopen(lambda request, timeout=None:
                           self._Response(b'{"status":0}'))
        self.assertFalse(self._notifier().send("t", "m", "normal"))


if __name__ == "__main__":
    unittest.main()
