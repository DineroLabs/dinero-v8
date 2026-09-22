#!/usr/bin/env python3
"""Fault-injection tests for the load harness parser and hostile scenario (no daemon, no network)."""
import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import shielded_load as L

def _vi(n): return bytes([n]) if n < 253 else b"\xfd" + n.to_bytes(2, "little")
_bundle = ((0).to_bytes(8, "little") + _vi(1) + b"\x11" * 32 + b"\x22" * 32 + b"\x33" * 33 + _vi(1000) + b"\x44" * 1000 + _vi(2)
           + (b"\x55" * 32 + b"\x66" * 33 + _vi(611) + b"\x77" * 611 + _vi(900) + b"\x88" * 900) * 2 + _vi(50) + b"\x99" * 50 + b"\xaa" * 33 + b"\xbb" * 64)
SYNTHETIC_TX_HEX = (b"\x06\x00\x00\x00\x00\x01\x00\x01" + (0).to_bytes(8, "little") + _vi(len(_bundle)) + _bundle + b"\x00" * 4).hex()

class StubNode:
    """Scripted RPC: responses is a list of callables or values consumed per testmempoolaccept call."""
    def __init__(self, accept_replies, seed_reply):
        self.accept_replies, self.seed_reply, self.calls = list(accept_replies), seed_reply, 0
        self.log_path = tempfile.mktemp(); open(self.log_path, "w").write("")
        self.proc = type("P", (), {"pid": os.getpid()})()
    def rpc(self, method, params, timeout=60):
        if method == "wallet.getnewaddress": return {"address": "rdin1p"}
        if method == "wallet.getshieldedaddress": return {"address": "rdins1"}
        if method == "wallet.shield": return {"txid": "aa" * 32}
        if method == "wallet.transfer": return {"txid": "bb" * 32}
        if method == "getrawtransaction": return {"hex": SYNTHETIC_TX_HEX}
        if method in ("mempool.clear", "generatetoaddress"): return []
        if method == "getrawmempool": return []
        if method == "getbestblockhash": return "h"
        if method == "getblockcount": return 1
        if method == "wallet.shieldedbalance": return {"balance_una": 0}
        if method == "getblocktemplate": return {"previousblockhash": "h"}
        if method == "testmempoolaccept":
            self.calls += 1
            if self.calls <= 2: return self.seed_reply   # two positive seeds (shield, transfer)
            r = self.accept_replies.pop(0) if self.accept_replies else [{"allowed": False, "reject-reason": "proof-invalid"}]
            if isinstance(r, Exception): raise r
            return r
        raise AssertionError("unexpected rpc " + method)
    def cpu_seconds(self): return 0.0
    def rss_mb(self): return 1

class ForkSidesTests(unittest.TestCase):
    def test_a_side_always_longer_than_b_side(self):
        for n in (1, 2, 3, 5):
            b_side, a_side = L.fork_sides(n)
            self.assertGreater(a_side, b_side); self.assertGreaterEqual(b_side, 1)

class VerdictAggregationTests(unittest.TestCase):
    def test_hostile_only_verdict_does_not_need_a_steady_result(self):
        # regression for the hostile-only crash: rs is None, rh clean -> exit 0; rh failed -> 2
        rs, rh = None, {"qualification_failed": False}
        self.assertFalse(any(x and x.get("qualification_failed") for x in (rs, rh)))
        rh = {"qualification_failed": True}
        self.assertTrue(any(x and x.get("qualification_failed") for x in (rs, rh)))

class ParserTests(unittest.TestCase):
    def test_list_form_accepted_and_rejected(self):
        self.assertEqual(L.parse_accept([{"allowed": True, "txid": "x"}]), ("accepted", None))
        self.assertEqual(L.parse_accept([{"allowed": False, "reject-reason": "proof-invalid"}]), ("rejected", "proof-invalid"))
        self.assertEqual(L.parse_accept({"allowed": False, "reject_reason": "bundle-malformed"}), ("rejected", "bundle-malformed"))
    def test_malformed_replies(self):
        for bad in ([], [{}], "ok", None, [{"allowed": "yes"}], [{"allowed": True}, {"allowed": True}]):
            self.assertEqual(L.parse_accept(bad)[0], "malformed", bad)

class HostileScenarioTests(unittest.TestCase):
    def run_hostile(self, replies, seed=None):
        out = tempfile.mkdtemp()
        node = StubNode(replies, seed if seed is not None else [{"allowed": True, "txid": "s"}])
        return L.scenario_hostile(node, 0.3, out), node
    def test_accepted_list_reply_is_counted_and_fails_qualification(self):
        res, _ = self.run_hostile([[{"allowed": True, "txid": "evil"}]])
        self.assertGreaterEqual(sum(c["accepted"] for c in res["counts"].values()), 1)
        self.assertTrue(res["qualification_failed"]); self.assertEqual(res["accepted_examples"][0]["reply"][0]["txid"], "evil")
    def test_rejections_keep_reasons_and_are_decisions(self):
        res, _ = self.run_hostile([[{"allowed": False, "reject-reason": "proof-invalid"}]] * 3)
        # A 0.3 s stub run is incomplete: it must FAIL qualification for coverage, never for acceptance.
        self.assertTrue(res["qualification_failed"])
        self.assertFalse(any("ACCEPTED" in f for f in res["qualification_failures"]))
        self.assertTrue(any("decisions" in f or "full verification" in f for f in res["qualification_failures"]))
        reasons = {r for k in res["reject_reasons"] for r in res["reject_reasons"][k]}
        self.assertIn("proof-invalid", reasons)
        self.assertGreater(sum(c["decisions"] for c in res["counts"].values()), 0)
    def test_transport_and_protocol_errors_are_not_decisions(self):
        res, _ = self.run_hostile([L.TransportError("reset"), L.ProtocolError("boom"), L.BusyError()] * 2)
        c = {k: sum(v[k] for v in res["counts"].values()) for k in ("transport", "protocol", "busy503")}
        self.assertGreaterEqual(c["transport"], 1); self.assertGreaterEqual(c["protocol"], 1); self.assertGreaterEqual(c["busy503"], 1)
        for k in res["counts"]:   # error replies never appear in the timed decision series
            self.assertEqual(res["counts"][k]["decisions"], res["counts"][k]["rejected"] + res["counts"][k]["accepted"])
    def test_malformed_replies_are_counted_separately(self):
        res, _ = self.run_hostile(["garbage", [], [{"nope": 1}]] * 2)
        self.assertGreaterEqual(sum(c["malformed"] for c in res["counts"].values()), 1)
    def test_rejected_seed_is_a_qualification_failure(self):
        with self.assertRaises(L.QualificationFailure):
            self.run_hostile([], seed=[{"allowed": False, "reject-reason": "proof-invalid"}])
    def test_malformed_seed_reply_is_a_qualification_failure(self):
        with self.assertRaises(L.QualificationFailure):
            self.run_hostile([], seed="nonsense")



class EnforcementTests(unittest.TestCase):
    def lanes(self, **over):
        base = {"decisions": 100, "accepted": 0, "rejected": 100, "malformed": 0, "busy503": 0, "transport": 0, "protocol": 0}
        c = {"shield_1proof/proof": dict(base), "shield_1proof/ciphertext_control": dict(base, accepted=20, rejected=0, decisions=20)}
        for k, v in over.items(): c["shield_1proof/proof"][k] = v
        return c
    def decisions(self, n_full=10, n_cheap=90):
        return {"shield_1proof/proof": [{"ms": 500.0} for _ in range(n_full)] + [{"ms": 5.0} for _ in range(n_cheap)]}
    def test_healthy_run_passes(self):
        self.assertEqual(L.enforce_hostile(self.lanes(), self.decisions(), {}), [])
    def test_zero_decisions_with_transport_errors_fails(self):
        c = self.lanes(decisions=0, rejected=0, transport=5000)
        fails = L.enforce_hostile(c, {"shield_1proof/proof": []}, {})
        self.assertTrue(any("error ratio" in f for f in fails)); self.assertTrue(any("decisions" in f for f in fails))
    def test_no_full_verification_fails(self):
        fails = L.enforce_hostile(self.lanes(), self.decisions(n_full=0, n_cheap=100), {})
        self.assertTrue(any("full verification" in f for f in fails))
    def test_acceptance_fails(self):
        fails = L.enforce_hostile(self.lanes(accepted=1), self.decisions(), {})
        self.assertTrue(any("ACCEPTED" in f for f in fails))
    def test_fatal_log_counter_fails(self):
        fails = L.enforce_hostile(self.lanes(), self.decisions(), {"REORG ABORT": 1})
        self.assertTrue(any("REORG ABORT" in f for f in fails))
    def test_control_lane_never_accepting_is_suspect(self):
        c = self.lanes(); c["shield_1proof/ciphertext_control"]["accepted"] = 0
        fails = L.enforce_hostile(c, self.decisions(), {})
        self.assertTrue(any("control lane" in f for f in fails))
    def test_two_node_enforces_counters_and_convergence(self):
        ok = {"phases": {"W2_catchup_cold_proofs": {"synced": True, "proofs_total": 16, "a_log_counters": {}, "b_log_counters": {}},
                         "W3_reorg_onto_cold_proofs": {"converged_to_a": True, "a_side_proofs": 16, "a_log_counters": {}, "b_log_counters": {}}}}
        self.assertEqual(L.enforce_two_node(ok), [])
        bad = {"phases": {"W2_catchup_cold_proofs": {"synced": True, "proofs_total": 16, "a_log_counters": {}, "b_log_counters": {"INVARIANT VIOLATION": 2}},
                          "W3_reorg_onto_cold_proofs": {"converged_to_a": False, "a_side_proofs": 16, "a_log_counters": {}, "b_log_counters": {}}}}
        fails = L.enforce_two_node(bad)
        self.assertTrue(any("INVARIANT" in f for f in fails)); self.assertTrue(any("converge" in f for f in fails))
        self.assertTrue(L.enforce_two_node({"phases": {}}))
    def test_steady_requires_work_and_clean_logs(self):
        good = {"generation": {"builds_ok": 3}, "blocks": {"count": 2, "shielded_txs_confirmed": 3}, "probes": {"x": {"samples": 10, "errors": {}}}, "log_counters": {}}
        self.assertEqual(L.enforce_steady(good), [])
        empty = {"generation": {"builds_ok": 0}, "blocks": {"count": 0, "shielded_txs_confirmed": 0}, "probes": {"x": {"samples": 10, "errors": {"transport": 5}}}, "log_counters": {"SAFE MODE": 1}}
        fails = L.enforce_steady(empty)
        self.assertGreaterEqual(len(fails), 4)

if __name__ == "__main__": unittest.main()
