import json
import subprocess
import unittest
from unittest import mock

from poller import SSHRPC, parse_safe_mode, comparison_heights, poll_cycle


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
        self.assertEqual(comparison_heights(heights, {}), {
            "a": {90, 100}, "b": {90, 100}, "c": {90},
        })

    def test_observer_pairs_with_every_voter_on_both_sides(self):
        """The comparison the rules actually make is observer-vs-MEMBER at the
        pairwise minimum. Asking only the observer, or asking against a median,
        leaves every such comparison UNDETERMINED and makes a forked observer
        invisible."""
        result = comparison_heights({"a": 100, "b": 90}, {"w": 95})
        self.assertEqual(result["w"], {90, 95})
        self.assertIn(95, result["a"], "the voter needs the observer's height too")
        self.assertIn(90, result["b"])


class TestPollCycle(unittest.TestCase):
    def test_stage_two_fetches_both_sides_of_every_comparison(self):
        """Stage 2's entire purpose is WHICH (node, height) pairs get fetched.
        Asserting only on the returned observations cannot see this."""
        nodes = [{"name": "a", "role": "voting"},
                 {"name": "b", "role": "voting"},
                 {"name": "w", "role": "observer"}]
        heights = {"a": 100, "b": 90, "w": 95}
        responses = {}
        for name, height in heights.items():
            responses[(name, "getdaemonstatus")] = {"result": {"height": height}}
            responses[(name, "blockchain.getbestblockhash")] = {"result": f"H{height}"}
        rpc = FakeRPC(responses)
        poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")

        asked = {(n, p[0]) for n, m, p in rpc.calls
                 if m == "blockchain.getblockhash"}
        # a-b pair at 90; w-a pair at 95; w-b pair at 90 — both sides each.
        for pair in (("a", 90), ("b", 90), ("w", 95), ("a", 95), ("w", 90)):
            self.assertIn(pair, asked, f"missing comparison hash for {pair}")


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

    def test_stage_two_is_abandoned_once_the_budget_elapses(self):
        """Abandoning stage 2 is safe: an absent hash reads as UNDETERMINED,
        which raises telemetry_degraded rather than a fork page. The budget
        exists so one hung node cannot push the cycle past the dead-man's
        overdue deadline — this proves the deadline is actually enforced
        against a fake clock, not just accepted as an unused parameter."""
        nodes = [{"name": "a", "role": "voting"},
                 {"name": "b", "role": "voting"},
                 {"name": "w", "role": "observer"}]
        heights = {"a": 100, "b": 90, "w": 95}
        responses = {}
        for name, height in heights.items():
            responses[(name, "getdaemonstatus")] = {"result": {"height": height}}
            responses[(name, "blockchain.getbestblockhash")] = {"result": f"H{height}"}
        rpc = FakeRPC(responses)

        # First call establishes the deadline; every call after it reports
        # time already past that deadline, so stage 2 must abandon before
        # issuing a single blockchain.getblockhash call.
        clock = iter([0.0] + [100.0] * 10)
        result = poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z",
                            budget=45.0, monotonic=lambda: next(clock))

        hash_calls = [c for c in rpc.calls if c[1] == "blockchain.getblockhash"]
        self.assertEqual(hash_calls, [], "budget elapsed before stage 2 started")
        self.assertEqual(len(result), 3, "stage 1 observations still returned")
        for observation in result:
            self.assertEqual(observation.hashes_at, {},
                             "absent hash, not a fabricated one")


class TestSSHRPCRestartId(unittest.TestCase):
    """A non-zero exit must yield None, never stdout.

    A forced-command wrapper that prints a constant string on failure (e.g.
    an error banner, or a stale cached value) would otherwise become a
    stable but BOGUS restart identity — masking every real restart forever.
    That is worse than no signal at all, because node_restart would look
    like it was working.
    """

    def test_non_zero_exit_yields_none_even_with_stdout(self):
        nodes = [{"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"}]
        rpc = SSHRPC(nodes)
        fake_result = subprocess.CompletedProcess(
            args=["ssh"], returncode=1, stdout=b"bogus-but-stable-id\n", stderr=b"")
        with mock.patch("poller.subprocess.run", return_value=fake_result):
            self.assertIsNone(rpc.restart_id("a"))

    def test_zero_exit_returns_the_stripped_stdout(self):
        nodes = [{"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"}]
        rpc = SSHRPC(nodes)
        fake_result = subprocess.CompletedProcess(
            args=["ssh"], returncode=0, stdout=b"real-invocation-id\n", stderr=b"")
        with mock.patch("poller.subprocess.run", return_value=fake_result):
            self.assertEqual(rpc.restart_id("a"), "real-invocation-id")

    def test_restart_id_sends_empty_stdin_now_that_dash_n_is_gone(self):
        """-n was removed from SSH_BASE (it fought the call() stdin path), so
        every invocation, including restart_id's, must now explicitly close
        stdin with input=b"" rather than relying on the flag."""
        nodes = [{"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"}]
        rpc = SSHRPC(nodes)
        fake_result = subprocess.CompletedProcess(
            args=["ssh"], returncode=0, stdout=b"id\n", stderr=b"")
        with mock.patch("poller.subprocess.run",
                        return_value=fake_result) as run:
            rpc.restart_id("a")
        self.assertEqual(run.call_args.kwargs.get("input"), b"")


class TestSSHRPCCall(unittest.TestCase):
    """The quoting-contract regression: the payload MUST travel on stdin, not
    as a shell-quoted argv element.

    This is the discriminating check for the bug the design went through
    three failed iterations to avoid: `set -- $SSH_ORIGINAL_COMMAND` word-
    splits without quote removal, `${SSH_ORIGINAL_COMMAND#rpc }` keeps the
    quotes literally, and `eval` executes attacker-influenced text. All three
    make every RPC fail to parse and every node report unreachable — a
    fleet-wide false outage caused by a one-line shell mistake. Putting the
    payload on stdin removes the parsing step entirely, so this test must
    fail against a `["rpc", shlex.quote(payload)]`-style argv and pass
    against the stdin-only version.
    """

    def test_payload_travels_on_stdin_not_argv(self):
        nodes = [{"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"}]
        rpc = SSHRPC(nodes)
        fake_result = subprocess.CompletedProcess(
            args=["ssh"], returncode=0,
            stdout=b'{"jsonrpc": "2.0", "id": "w", "result": {"height": 1}}',
            stderr=b"")
        with mock.patch("poller.subprocess.run",
                        return_value=fake_result) as run:
            rpc.call("a", "getdaemonstatus")

        argv = run.call_args.args[0]
        kwargs = run.call_args.kwargs

        # The command ends with the bare literal "rpc" — nothing after it.
        # A regression to the old argument-passing form would append a
        # second, shell-quoted element here.
        self.assertEqual(argv[-1], "rpc")
        self.assertNotIn("-n", argv)

        payload = json.loads(kwargs["input"].decode())
        self.assertEqual(payload["method"], "getdaemonstatus")


if __name__ == "__main__":
    unittest.main()
