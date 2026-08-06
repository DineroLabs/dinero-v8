import unittest

from poller import parse_safe_mode, comparison_heights, poll_cycle


class FakeRPC:
    """Records calls and replays canned responses keyed by (node, method)."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def call(self, node, method, params=None):
        self.calls.append((node, method, tuple(params or ())))
        value = self.responses.get((node, method))
        if isinstance(value, Exception):
            raise value
        return value


class TestParseSafeMode(unittest.TestCase):
    def test_active_is_active(self):
        self.assertEqual(
            parse_safe_mode({"result": {"active": True, "reason": "bad root"}}),
            ("active", "bad root"))

    def test_inactive_is_inactive(self):
        self.assertEqual(
            parse_safe_mode({"result": {"active": False, "reason": ""}}),
            ("inactive", None))

    def test_method_not_found_is_unknown_never_inactive(self):
        """An older daemon must not read as healthy."""
        response = {"error": {"code": -32601, "message": "Method not found"}}
        self.assertEqual(parse_safe_mode(response), ("unknown", None))

    def test_malformed_response_is_unknown(self):
        self.assertEqual(parse_safe_mode({"result": {}}), ("unknown", None))
        self.assertEqual(parse_safe_mode(None), ("unknown", None))


class TestComparisonHeights(unittest.TestCase):
    def test_pairwise_minimum_for_voters(self):
        heights = {"a": 100, "b": 105, "c": 90}
        self.assertEqual(comparison_heights(heights, {}, None), {
            "a": {90, 100}, "b": {90, 100}, "c": {90},
        })

    def test_observer_compared_against_quorum_median(self):
        result = comparison_heights({"a": 100}, {"w": 95}, quorum_median=100)
        self.assertIn(95, result["w"])


class TestPollCycle(unittest.TestCase):
    def test_unreachable_node_still_produces_an_observation(self):
        nodes = [{"name": "a", "role": "voting"}]
        rpc = FakeRPC({("a", "getdaemonstatus"): TimeoutError("no route")})
        result = poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")
        self.assertEqual(len(result), 1)
        self.assertFalse(result[0].reachable)
        self.assertEqual(result[0].safe_mode, "unknown")

    def test_reachable_means_core_rpc_answered_not_every_field(self):
        nodes = [{"name": "a", "role": "voting"}]
        rpc = FakeRPC({
            ("a", "getdaemonstatus"): {"result": {"height": 100}},
            ("a", "blockchain.getbestblockhash"): {"result": "H100"},
            ("a", "safemode.status"): {"error": {"code": -32601}},
        })
        result = poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")
        self.assertTrue(result[0].reachable, "core RPCs answered")
        self.assertEqual(result[0].safe_mode, "unknown")


if __name__ == "__main__":
    unittest.main()
