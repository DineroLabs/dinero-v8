# tools/fleet-watcher/tests/test_store.py
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
        # Frozen clock: the store's timestamps and the tests' synthetic `now`
        # values must live in ONE time domain, or assertions about deadlines
        # test nothing.
        self.store = Store(self.path, clock=lambda: 0.0)

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

    def test_rollback_happens_inside_the_transaction_not_before_it(self):
        """The type-check test above fails BEFORE the transaction opens, so it
        proves nothing about atomicity. This one passes validation and dies at
        SQLite bind time, inside executemany, which is the real rollback path."""
        good = obs("a")
        unbindable = Observation(
            cycle_id="c1", timestamp="2026-08-06T00:00:00Z", node="b",
            role="voting", reachable=True, height=object(), tip_hash="H",
            hashes_at={100: "H100"}, peers_in=1, peers_out=1, synced=True,
            safe_mode="inactive", safe_mode_reason=None, restart_id="r1",
        )
        with self.assertRaises(Exception):
            self.store.write_cycle([good, unbindable])
        self.assertEqual(self.store.cycle("c1"), [],
                         "row one must roll back with row two")

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

    def test_only_one_open_incident_per_rule(self):
        a = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        b = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertEqual(a, b)
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_a_different_node_set_reuses_the_open_incident_and_updates_it(self):
        """Membership is a mutable fact about an open incident, not identity."""
        a = self.store.open_incident("node_unreachable", ("a",), "normal", "a down")
        b = self.store.open_incident("node_unreachable", ("a", "b"), "normal", "a,b down")
        self.assertEqual(a, b)
        only = self.store.open_incidents()[0]
        self.assertEqual(only.nodes, ("a", "b"))
        self.assertEqual(only.detail, "a,b down")

    def test_overdue_critical_detects_stuck_emergency_item(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertFalse(self.store.has_overdue_critical(now=0.0, deadline=300.0))
        self.assertTrue(self.store.has_overdue_critical(now=1000.0, deadline=300.0))

    def test_sent_item_is_never_overdue(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.assertFalse(self.store.has_overdue_critical(now=1e9, deadline=300.0))

    def test_closing_twice_enqueues_only_one_recovery(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        self.store.close_incident(iid)
        recoveries = [i for i in self.store.pending_outbox(now=0.0)
                      if i.kind == "recovery"]
        self.assertEqual(len(recoveries), 1)

    def test_open_incidents_nodes_round_trip_sorted(self):
        """open_incident stores sorted nodes, so open_incidents() returns them
        sorted. Any consumer keying on (rule, nodes) must sort too, or its keys
        never match and incidents never close."""
        self.store.open_incident("node_behind", ("c", "a"), "normal", "x")
        self.assertEqual(self.store.open_incidents()[0].nodes, ("a", "c"))

    def test_failed_delivery_is_hidden_until_its_backoff_elapses(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        item = self.store.pending_outbox(now=0.0)[0]
        self.store.mark_failed(item.outbox_id, backoff_seconds=30.0)
        self.assertEqual(self.store.pending_outbox(now=10.0), [])
        due = self.store.pending_outbox(now=40.0)
        self.assertEqual(len(due), 1)
        self.assertEqual(due[0].attempts, 1)

    def test_overdue_deadline_actually_depends_on_the_deadline(self):
        """Guards the regression where created_at and now lived in different
        time domains, making this gate a constant."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertFalse(self.store.has_overdue_critical(now=100.0, deadline=300.0))
        self.assertTrue(self.store.has_overdue_critical(now=400.0, deadline=300.0))

    def test_outbox_survives_reopen(self):
        """The crash window: incident opened, process dies before delivery."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        reopened = Store(self.path, clock=lambda: 0.0)
        self.assertEqual(len(reopened.pending_outbox(now=0.0)), 1)


if __name__ == "__main__":
    unittest.main()
