import os
import tempfile
import unittest

from models import Observation
from store import Store


def obs(node, cycle="c1"):
    return Observation(
        cycle_id=cycle, timestamp="2026-08-06T00:00:00Z", node=node,
        role="voting", reachable=True, height=100, tip_hash="H",
        hashes_at={100: "H100"}, peers_in=1, peers_out=1, synced=True,
        safe_mode="inactive", safe_mode_reason=None, restart_id="r1",
    )


class TestStore(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path)

    def tearDown(self):
        os.unlink(self.path)

    def test_cycle_writes_all_observations(self):
        self.store.write_cycle([obs("a"), obs("b"), obs("c")])
        self.assertEqual(len(self.store.cycle("c1")), 3)

    def test_partial_cycle_is_not_visible_when_write_fails(self):
        """Rules must never evaluate half a cycle."""
        bad = [obs("a"), "not-an-observation"]
        with self.assertRaises(Exception):
            self.store.write_cycle(bad)
        self.assertEqual(self.store.cycle("c1"), [])

    def test_hashes_at_round_trips(self):
        self.store.write_cycle([obs("a")])
        self.assertEqual(self.store.cycle("c1")[0].hashes_at, {100: "H100"})

    def test_open_incident_enqueues_notification_atomically(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertEqual(len(self.store.open_incidents()), 1)
        pending = self.store.pending_outbox(now=0.0)
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0].incident_id, iid)
        self.assertEqual(pending[0].kind, "open")

    def test_close_incident_enqueues_recovery(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        self.assertEqual(self.store.open_incidents(), [])
        self.assertEqual(self.store.pending_outbox(now=0.0)[0].kind, "recovery")

    def test_only_one_open_incident_per_rule_and_nodes(self):
        a = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        b = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertEqual(a, b)
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_overdue_critical_detects_stuck_emergency_item(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertFalse(self.store.has_overdue_critical(now=0.0, deadline=300.0))
        self.assertTrue(self.store.has_overdue_critical(now=1000.0, deadline=300.0))

    def test_sent_item_is_never_overdue(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.assertFalse(self.store.has_overdue_critical(now=1e9, deadline=300.0))

    def test_outbox_survives_reopen(self):
        """The crash window: incident opened, process dies before delivery."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        reopened = Store(self.path)
        self.assertEqual(len(reopened.pending_outbox(now=0.0)), 1)


if __name__ == "__main__":
    unittest.main()
