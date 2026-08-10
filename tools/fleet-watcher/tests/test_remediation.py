import json
import os
import tempfile
import unittest

from models import Incident, Observation
from remediation import eligible_incident, emit_pending
from store import Store


def observation(node, height, hashes):
    return Observation(
        cycle_id="c", timestamp="2026-08-10T00:00:00Z", node=node,
        role="voting", reachable=True, height=height,
        tip_hash=hashes.get(height), hashes_at=hashes,
        peers_in=1, peers_out=2, synced=True, safe_mode="inactive",
        safe_mode_reason=None, restart_id="r1")


def incident(node="local"):
    return Incident("a" * 32, "node_behind", (node,), "normal",
                    "behind", "2026-08-10T00:00:00Z")


class TestEligibility(unittest.TestCase):
    def setUp(self):
        self.behind = [
            observation("local", 80, {80: "H80"}),
            observation("b", 100, {80: "H80", 100: "H100"}),
            observation("c", 100, {80: "H80", 100: "H100"}),
        ]

    def test_two_agreeing_voters_ten_blocks_ahead_qualifies(self):
        self.assertEqual(
            eligible_incident(self.behind, [incident()], "local", 10),
            "a" * 32)

    def test_a_quiet_whole_fleet_never_qualifies(self):
        equal = [observation(n, 100, {100: "H100"})
                 for n in ("local", "b", "c")]
        self.assertIsNone(eligible_incident(equal, [incident()], "local", 10))

    def test_a_confirmed_fork_never_auto_restarts(self):
        forked = list(self.behind)
        forked[0] = observation("local", 80, {80: "OTHER"})
        self.assertIsNone(eligible_incident(forked, [incident()], "local", 10))

    def test_missing_comparison_hash_never_auto_restarts(self):
        incomplete = list(self.behind)
        incomplete[1] = observation("b", 100, {100: "H100"})
        self.assertIsNone(
            eligible_incident(incomplete, [incident()], "local", 10))

    def test_two_voter_fleet_is_not_enough_for_automatic_repair(self):
        self.assertIsNone(
            eligible_incident(self.behind[:2], [incident()], "local", 10))

    def test_rule_evidence_without_an_open_incident_is_not_enough(self):
        self.assertIsNone(eligible_incident(self.behind, [], "local", 10))


class TestRemediationQueue(unittest.TestCase):
    def setUp(self):
        fd, self.db = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.unlink, self.db)
        self.store = Store(self.db, clock=lambda: 123.0)

    def test_request_is_durable_atomic_and_submitted_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "restart.json")
            iid = self.store.open_incident(
                "node_behind", ("local",), "normal", "behind")
            self.store.enqueue_remediation(iid, "local")
            self.assertEqual(emit_pending(self.store, path), 1)
            with open(path, encoding="utf-8") as handle:
                payload = json.load(handle)
            self.assertEqual(payload["incident_id"], iid)
            self.assertEqual(payload["node"], "local")
            self.assertEqual(payload["requested_at"], 123.0)
            self.assertEqual(self.store.pending_remediations(), [])
            self.store.enqueue_remediation(iid, "local")
            self.assertEqual(emit_pending(self.store, path), 0)


if __name__ == "__main__":
    unittest.main()
