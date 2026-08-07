import unittest
from models import Observation
from rules import (AGREE, DISAGREE, UNDETERMINED, compatible, compute_quorum,
                   evaluate, undetermined_voter_pairs)


def obs(node, role="voting", height=100, tip="tip", hashes=None, reachable=True):
    return Observation(
        cycle_id="c", timestamp="t", node=node, role=role, reachable=reachable,
        height=height, tip_hash=tip, hashes_at=hashes or {}, peers_in=1,
        peers_out=1, synced=True, safe_mode="inactive", safe_mode_reason=None,
        restart_id="r1",
    )


class TestCompatible(unittest.TestCase):
    def test_same_hash_at_shared_height_agrees(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), AGREE)

    def test_one_block_ahead_agrees(self):
        """The false positive this whole design exists to avoid."""
        a = obs("a", height=100, tip="H100", hashes={100: "H100"})
        b = obs("b", height=101, tip="H101", hashes={100: "H100"})
        self.assertEqual(compatible(a, b), AGREE)

    def test_different_hash_at_shared_height_disagrees(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "FORK"})
        self.assertEqual(compatible(a, b), DISAGREE)

    def test_missing_comparison_hash_is_undetermined_not_agreement(self):
        """A missing signal must never synthesise agreement — but it is also
        not a fork, and calling it one pages on a healthy fleet."""
        a = obs("a", height=100, hashes={})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), UNDETERMINED)

    def test_unreachable_node_is_undetermined(self):
        a = obs("a", reachable=False, height=None, hashes={})
        b = obs("b", height=100, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), UNDETERMINED)


class TestComputeQuorum(unittest.TestCase):
    def test_three_agreeing_voters_form_quorum(self):
        o = [obs(n, height=100 + i, hashes={100: "H100", 101: "H101"})
             for i, n in enumerate(("a", "b", "c"))]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b", "c"})
        self.assertEqual(q.median_height, 101)

    def test_two_of_three_form_quorum_when_one_forks(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("b", height=100, hashes={100: "H100"}),
            obs("c", height=100, hashes={100: "FORK"}),
        ]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_observers_never_count_toward_quorum(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("obs1", role="observer", height=100, hashes={100: "H100"}),
        ]
        self.assertIsNone(compute_quorum(o), "one voter cannot form a quorum")

    def test_no_unique_largest_group_means_no_quorum(self):
        """2 v 2 — two equally sized competing groups."""
        o = [
            obs("a", height=100, hashes={100: "X"}),
            obs("b", height=100, hashes={100: "X"}),
            obs("c", height=100, hashes={100: "Y"}),
            obs("d", height=100, hashes={100: "Y"}),
        ]
        self.assertIsNone(compute_quorum(o))

    def test_all_voters_disagree_means_no_quorum(self):
        o = [obs(n, height=100, hashes={100: h})
             for n, h in (("a", "X"), ("b", "Y"), ("c", "Z"))]
        self.assertIsNone(compute_quorum(o))

    def test_majority_of_three_beats_minority_of_two(self):
        """A larger group wins outright — this is not a tie."""
        o = [obs(n, height=100, hashes={100: "X"}) for n in ("a", "b", "c")]
        o += [obs(n, height=100, hashes={100: "Y"}) for n in ("d", "e")]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b", "c"})

    def test_unreachable_voter_is_excluded_from_the_group_search(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", reachable=False, height=None, hashes={})]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_agreeing_observer_is_not_added_to_the_quorum(self):
        """Stronger than a bare count check: two voters DO form a quorum here,
        so passing requires the observer to be excluded by role, not by size."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("w", role="observer", height=100, hashes={100: "X"})]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_median_height_on_an_even_member_count(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=200, hashes={100: "X"})]
        self.assertEqual(compute_quorum(o).median_height, 200)

    def test_string_keyed_hashes_do_not_silently_agree(self):
        """Guards a JSON round-trip that forgets to coerce keys back to int."""
        a = obs("a", height=100, hashes={"100": "X"})
        b = obs("b", height=100, hashes={100: "X"})
        self.assertEqual(compatible(a, b), UNDETERMINED)


class TestUndeterminedPairs(unittest.TestCase):
    def test_the_non_transitive_chain_reports_undetermined_not_a_fork(self):
        """a, b, c are all on the SAME chain, but a and c share no hash.

        Under a binary rule this produced two tied cliques and therefore an
        emergency consensus page on a healthy fleet. It must be reported as a
        telemetry gap instead.
        """
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        self.assertIsNone(compute_quorum(o))
        self.assertIn(("a", "c"), undetermined_voter_pairs(o))

    def test_a_genuine_fork_reports_no_undetermined_pairs(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=100, hashes={100: "FORK"})]
        self.assertEqual(undetermined_voter_pairs(o), [])


def rules_fired(hits):
    return {h.rule for h in hits}


class TestEvaluate(unittest.TestCase):
    def _healthy(self):
        return [obs(n, height=100, hashes={100: "H100"}) for n in ("a", "b", "c")]

    def test_healthy_fleet_fires_nothing(self):
        self.assertEqual(rules_fired(evaluate(self._healthy(), 3)), set())

    def test_one_block_apart_fires_nothing(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("b", height=101, hashes={100: "H100"}),
            obs("c", height=100, hashes={100: "H100"}),
        ]
        self.assertEqual(rules_fired(evaluate(o, 3)), set())

    def test_safe_mode_fires_for_any_node(self):
        o = self._healthy()
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "active",
                              "safe_mode_reason": "bad-utreexo-root"})
        self.assertIn("safe_mode", rules_fired(evaluate(o, 3)))

    def test_unknown_safe_mode_fires_telemetry_not_safe_mode(self):
        o = self._healthy()
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "unknown"})
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("telemetry_degraded", fired)
        self.assertNotIn("safe_mode", fired)

    def test_tip_divergence_fires_on_fork(self):
        o = self._healthy()
        o[2] = Observation(**{**o[2].__dict__, "hashes_at": {100: "FORK"}})
        self.assertIn("tip_divergence", rules_fired(evaluate(o, 3)))

    def test_no_quorum_from_real_disagreement_fires_consensus_health(self):
        o = [obs(n, height=100, hashes={100: h})
             for n, h in (("a", "X"), ("b", "Y"), ("c", "Z"))]
        self.assertIn("consensus_health", rules_fired(evaluate(o, 3)))

    def test_no_quorum_from_missing_hashes_fires_telemetry_not_consensus(self):
        """The healthy-fleet false page. All three are on the same chain."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("telemetry_degraded", fired)
        self.assertNotIn("consensus_health", fired)

    def test_uncomparable_observer_is_not_reported_as_diverged(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", height=50, hashes={}))
        self.assertNotIn("observer_divergence", rules_fired(evaluate(o, 3)))

    def test_majority_unreachable_suppresses_consensus_health(self):
        """It names the cause rather than the symptom."""
        o = [obs("a", height=100, hashes={100: "H100"}),
             obs("b", reachable=False, height=None, hashes={}),
             obs("c", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("majority_unreachable", fired)
        self.assertNotIn("consensus_health", fired)

    def test_node_behind_fires_beyond_threshold(self):
        o = [obs("a", height=200, hashes={100: "H100", 200: "H200"}),
             obs("b", height=200, hashes={100: "H100", 200: "H200"}),
             obs("c", height=100, hashes={100: "H100"})]
        self.assertIn("node_behind", rules_fired(evaluate(o, 3)))

    def test_observer_divergence_fires_without_touching_quorum(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", height=100, hashes={100: "FORK"}))
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("observer_divergence", fired)
        self.assertNotIn("consensus_health", fired)
        self.assertNotIn("tip_divergence", fired)

    def test_observer_down_never_affects_quorum(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", reachable=False,
                     height=None, hashes={}))
        self.assertEqual(rules_fired(evaluate(o, 3)), set())

    def test_uncomparable_voter_does_not_fire_tip_divergence(self):
        """A quorum exists and one voter simply cannot be compared. That is a
        telemetry gap, not a fork, and must not raise an emergency page."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        fired = rules_fired(evaluate(o, 3))
        self.assertNotIn("tip_divergence", fired)
        self.assertIn("telemetry_degraded", fired)

    def test_voting_node_absent_from_cycle_does_not_fire_consensus_health(self):
        """A collector gap must not masquerade as a fork."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertNotIn("consensus_health", fired)
        self.assertIn("telemetry_degraded", fired)

    def test_single_unreachable_voter_fires_node_unreachable(self):
        o = self._healthy()
        o[2] = obs("c", reachable=False, height=None, hashes={})
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("node_unreachable", fired)
        self.assertNotIn("majority_unreachable", fired)

    def test_majority_unreachable_suppresses_node_unreachable(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", reachable=False, height=None, hashes={}),
             obs("c", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("majority_unreachable", fired)
        self.assertNotIn("node_unreachable", fired)

    def test_three_of_five_unreachable_is_a_majority(self):
        """The older threshold stayed silent here — 2 survivors formed a
        'quorum' that was not a majority of the fleet."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"})]
        o += [obs(n, reachable=False, height=None, hashes={})
              for n in ("c", "d", "e")]
        self.assertIn("majority_unreachable", rules_fired(evaluate(o, 5)))

    def test_observer_diverging_from_a_subset_of_quorum_still_fires(self):
        """DISAGREE with one member and uncomparable with another is still a
        different chain — quorum members agree with each other by construction."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=100, hashes={100: "X"})]
        o.append(obs("w", role="observer", height=100, hashes={100: "FORK"}))
        self.assertIn("observer_divergence", rules_fired(evaluate(o, 3)))

    def test_node_behind_boundary_is_inclusive_at_ten(self):
        base = [obs(n, height=110, hashes={100: "X", 110: "X"})
                for n in ("a", "b")]
        at_ten = base + [obs("c", height=100, hashes={100: "X"})]
        self.assertIn("node_behind", rules_fired(evaluate(at_ten, 3)))
        at_nine = base + [obs("c", height=101, hashes={100: "X", 101: "X"})]
        self.assertNotIn("node_behind", rules_fired(evaluate(at_nine, 3)))

    def test_telemetry_degraded_is_emitted_at_most_once_per_cycle(self):
        """Two keys for one condition would let a shifting node set reset the
        engine's counter, so a persistent gap would never open an incident."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "unknown"})
        hits = [h for h in evaluate(o, 3) if h.rule == "telemetry_degraded"]
        self.assertEqual(len(hits), 1)

    def test_node_restart_detected_from_changed_restart_id(self):
        prev = self._healthy()
        cur = [Observation(**{**x.__dict__, "restart_id": "r2"}) for x in prev]
        self.assertIn("node_restart",
                      rules_fired(evaluate(cur, 3, previous=prev)))

    def test_a_fork_pages_even_when_no_quorum_forms(self):
        """A confirmed fork must not be downgraded because a third node dropped
        an RPC. The DISAGREE evidence is in hand either way."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={}),          # one missing getblockhash
             obs("c", height=100, hashes={100: "Y"})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("tip_divergence", fired)

    def test_tip_divergence_names_the_disagreeing_nodes(self):
        o = self._healthy()
        o[2] = Observation(**{**o[2].__dict__, "hashes_at": {100: "FORK"}})
        hit = [h for h in evaluate(o, 3) if h.rule == "tip_divergence"][0]
        self.assertIn("c", hit.nodes)


if __name__ == "__main__":
    unittest.main()
