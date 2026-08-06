# tools/fleet-watcher/tests/test_models.py
import unittest
from models import Observation, RuleHit


class TestObservation(unittest.TestCase):
    def test_unreachable_observation_has_null_measurements(self):
        obs = Observation(
            cycle_id="c1", timestamp="2026-08-06T00:00:00Z", node="n1",
            role="voting", reachable=False, height=None, tip_hash=None,
            hashes_at={}, peers_in=None, peers_out=None, synced=None,
            safe_mode="unknown", safe_mode_reason=None, restart_id=None,
        )
        self.assertFalse(obs.reachable)
        self.assertIsNone(obs.height)
        self.assertEqual(obs.safe_mode, "unknown")

    def test_safe_mode_rejects_unknown_values(self):
        with self.assertRaises(ValueError):
            Observation(
                cycle_id="c1", timestamp="t", node="n1", role="voting",
                reachable=True, height=1, tip_hash="aa", hashes_at={},
                peers_in=0, peers_out=0, synced=True,
                safe_mode="maybe", safe_mode_reason=None, restart_id=None,
            )

    def test_role_rejects_unknown_values(self):
        with self.assertRaises(ValueError):
            Observation(
                cycle_id="c1", timestamp="t", node="n1", role="auditor",
                reachable=True, height=1, tip_hash="aa", hashes_at={},
                peers_in=0, peers_out=0, synced=True,
                safe_mode="inactive", safe_mode_reason=None, restart_id=None,
            )


class TestRuleHit(unittest.TestCase):
    def test_rule_hit_is_hashable_for_set_comparison(self):
        a = RuleHit(rule="safe_mode", nodes=("n1",), detail="x")
        b = RuleHit(rule="safe_mode", nodes=("n1",), detail="x")
        self.assertEqual({a}, {b})


if __name__ == "__main__":
    unittest.main()
