import unittest
from models import Observation
from rules import compute_quorum, compatible


def obs(node, role="voting", height=100, tip="tip", hashes=None, reachable=True):
    return Observation(
        cycle_id="c", timestamp="t", node=node, role=role, reachable=reachable,
        height=height, tip_hash=tip, hashes_at=hashes or {}, peers_in=1,
        peers_out=1, synced=True, safe_mode="inactive", safe_mode_reason=None,
        restart_id="r1",
    )


class TestCompatible(unittest.TestCase):
    def test_same_hash_at_shared_height_is_compatible(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertTrue(compatible(a, b))

    def test_one_block_ahead_is_compatible(self):
        """The false positive this whole design exists to avoid."""
        a = obs("a", height=100, tip="H100", hashes={100: "H100"})
        b = obs("b", height=101, tip="H101", hashes={100: "H100"})
        self.assertTrue(compatible(a, b))

    def test_different_hash_at_shared_height_is_incompatible(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "FORK"})
        self.assertFalse(compatible(a, b))

    def test_missing_comparison_hash_is_not_compatible(self):
        """A missing signal must never synthesise agreement."""
        a = obs("a", height=100, hashes={})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertFalse(compatible(a, b))

    def test_unreachable_node_is_never_compatible(self):
        a = obs("a", reachable=False, height=None, hashes={})
        b = obs("b", height=100, hashes={100: "H100"})
        self.assertFalse(compatible(a, b))


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


if __name__ == "__main__":
    unittest.main()
