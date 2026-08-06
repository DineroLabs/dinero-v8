import unittest
from models import Observation
from rules import (AGREE, DISAGREE, UNDETERMINED, compatible, compute_quorum,
                   undetermined_voter_pairs)


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


if __name__ == "__main__":
    unittest.main()
