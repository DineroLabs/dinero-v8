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

    def test_normal_severity_for_observer_divergence(self):
        hit = RuleHit("observer_divergence", ("w",), "forked")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(self.store.open_incidents()[0].severity, "normal")


if __name__ == "__main__":
    unittest.main()
