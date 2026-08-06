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


if __name__ == "__main__":
    unittest.main()
