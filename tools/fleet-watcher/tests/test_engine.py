# tools/fleet-watcher/tests/test_engine.py
import os
import tempfile
import unittest

from engine import Engine
from models import RuleHit
from store import Store

SAFE = RuleHit("safe_mode", ("a",), "halted")
BEHIND = RuleHit("node_behind", ("c",), "12 blocks")


class TestEngine(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path, clock=lambda: 0.0)
        self.engine = Engine(self.store, open_after=3, close_after=3)

    def tearDown(self):
        os.unlink(self.path)

    def test_ordinary_rule_needs_three_cycles_to_open(self):
        self.engine.process([BEHIND]); self.engine.process([BEHIND])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([BEHIND])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_safe_mode_opens_immediately(self):
        self.engine.process([SAFE])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_intermittent_condition_never_opens(self):
        """One good cycle resets the count — this is the anti-flap property."""
        for _ in range(5):
            self.engine.process([BEHIND])
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_incident_needs_three_healthy_cycles_to_close(self):
        self.engine.process([SAFE])
        self.engine.process([]); self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)
        self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_safe_mode_does_not_flap_closed_on_one_good_cycle(self):
        self.engine.process([SAFE])
        self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_recurrence_during_close_countdown_keeps_incident_open(self):
        self.engine.process([SAFE])
        self.engine.process([]); self.engine.process([SAFE]); self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_emergency_severity_for_chain_integrity_rules(self):
        self.engine.process([SAFE])
        self.assertEqual(self.store.open_incidents()[0].severity, "emergency")

    def test_drifting_node_set_still_opens_after_three_cycles(self):
        """A fleet always missing SOME node, never the same one three cycles
        running, previously opened nothing at all."""
        for nodes in (("a",), ("a", "b"), ("b",)):
            self.engine.process([RuleHit("telemetry_degraded", nodes, "gap")])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_widening_node_set_updates_membership_not_a_second_incident(self):
        for _ in range(3):
            self.engine.process([RuleHit("node_unreachable", ("a",), "a down")])
        self.engine.process([RuleHit("node_unreachable", ("a", "b"), "a,b down")])
        incidents = self.store.open_incidents()
        self.assertEqual(len(incidents), 1)
        self.assertEqual(incidents[0].nodes, ("a", "b"))
        self.assertEqual(incidents[0].detail, "a,b down")

    def test_unsorted_node_tuple_still_closes(self):
        """The store persists nodes sorted; a naive key comparison against the
        raw tuple would never match and the incident would never close."""
        hit = RuleHit("node_behind", ("b", "a"), "behind")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)
        for _ in range(3):
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_emergency_but_not_immediate_rule_still_needs_three_cycles(self):
        hit = RuleHit("consensus_health", ("a", "b"), "no quorum")
        self.engine.process([hit]); self.engine.process([hit])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_silent_rule_is_recorded_as_an_incident(self):
        """'Recorded but never notified' — the engine records it; the delivery
        worker is what declines to send."""
        hit = RuleHit("node_restart", ("a",), "restart id changed")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_steady_condition_opens_exactly_one_incident_and_one_notification(self):
        hit = RuleHit("node_behind", ("c",), "12 blocks")
        for _ in range(7):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)
        opens = [i for i in self.store.pending_outbox(now=0.0) if i.kind == "open"]
        self.assertEqual(len(opens), 1)

    def test_close_then_recur_costs_the_full_open_threshold_again(self):
        hit = RuleHit("node_behind", ("c",), "12 blocks")
        for _ in range(3):
            self.engine.process([hit])
        for _ in range(3):
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit]); self.engine.process([hit])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_normal_severity_for_observer_divergence(self):
        hit = RuleHit("observer_divergence", ("w",), "forked")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(self.store.open_incidents()[0].severity, "normal")


if __name__ == "__main__":
    unittest.main()
