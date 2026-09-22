#!/usr/bin/env python3
"""The enforcer must exit non-zero for every failure class, using fixture evidence trees; and the good
tree must pass. Runs the real script as a subprocess so the exit code itself is what is tested."""
import json, os, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); ENF = os.path.join(HERE, "enforce_outcomes.py")

def good_tree(root):
    os.makedirs(os.path.join(root, "hostile")); os.makedirs(os.path.join(root, "twonode")); os.makedirs(os.path.join(root, "steady"))
    open(os.path.join(root, "exit-codes.txt"), "w").write("hostile exit=0\ntwonode exit=0\nsteady exit=0\n")
    hostile = {"counts": {"shield_1proof/proof": {"decisions": 100, "accepted": 0, "rejected": 100, "malformed": 0, "busy503": 7, "transport": 0, "protocol": 0},
                          "shield_1proof/ciphertext_control": {"decisions": 20, "accepted": 20, "rejected": 0, "malformed": 0, "busy503": 0, "transport": 0, "protocol": 0}},
               "log_counters": {"SAFE MODE": 0}, "qualification_failed": False, "qualification_failures": []}
    two = {"phases": {"W2_catchup_cold_proofs": {"synced": True, "proofs_total": 16, "a_log_counters": {}, "b_log_counters": {}},
                      "W3_reorg_onto_cold_proofs": {"converged_to_a": True, "a_side_proofs": 16, "a_log_counters": {}, "b_log_counters": {}}},
           "qualification_failed": False, "qualification_failures": []}
    steady = {"blocks": {"shielded_txs_confirmed": 9}, "log_counters": {}, "qualification_failed": False, "qualification_failures": []}
    json.dump(hostile, open(os.path.join(root, "hostile", "hostile.json"), "w")); json.dump({"exit": 0}, open(os.path.join(root, "hostile", "OUTCOME.json"), "w"))
    json.dump(two, open(os.path.join(root, "twonode", "two_node.json"), "w")); json.dump({"exit": 0}, open(os.path.join(root, "twonode", "OUTCOME.json"), "w"))
    json.dump(steady, open(os.path.join(root, "steady", "steady.json"), "w")); json.dump(hostile, open(os.path.join(root, "steady", "hostile.json"), "w")); json.dump({"exit": 0}, open(os.path.join(root, "steady", "OUTCOME.json"), "w"))

def run(root):
    p = subprocess.run([sys.executable, ENF, root], capture_output=True, text=True); return p.returncode, p.stdout

class EnforcerExitCodeTests(unittest.TestCase):
    def setUp(self): self.root = tempfile.mkdtemp(); good_tree(self.root)
    def edit(self, rel, fn):
        p = os.path.join(self.root, rel); d = json.load(open(p)); fn(d); json.dump(d, open(p, "w"))
    def test_good_tree_passes_and_busy503_is_not_an_error(self):
        rc, out = run(self.root); self.assertEqual(rc, 0, out); self.assertIn("PASS", out)
    def test_missing_required_file_fails(self):
        os.remove(os.path.join(self.root, "twonode", "two_node.json")); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("missing required file", out)
    def test_malformed_json_fails(self):
        open(os.path.join(self.root, "hostile", "hostile.json"), "w").write("{not json"); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("malformed", out)
    def test_accepted_proof_fails_even_if_flag_says_pass(self):
        self.edit("hostile/hostile.json", lambda d: d["counts"]["shield_1proof/proof"].__setitem__("accepted", 1)); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("accepted", out)
    def test_invariant_counter_fails(self):
        self.edit("twonode/two_node.json", lambda d: d["phases"]["W2_catchup_cold_proofs"]["b_log_counters"].__setitem__("INVARIANT VIOLATION", 1)); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("INVARIANT", out)
    def test_failed_sync_or_reorg_fails(self):
        self.edit("twonode/two_node.json", lambda d: d["phases"]["W3_reorg_onto_cold_proofs"].__setitem__("converged_to_a", False)); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("converge", out)
    def test_flag_true_fails(self):
        self.edit("steady/steady.json", lambda d: d.__setitem__("qualification_failed", True)); rc, out = run(self.root); self.assertEqual(rc, 1)
    def test_nonzero_harness_exit_fails(self):
        open(os.path.join(self.root, "exit-codes.txt"), "a").write("hostile exit=2\n"); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("harness exit 2", out)
    def test_outcome_exit_nonzero_fails(self):
        self.edit("hostile/OUTCOME.json", lambda d: d.__setitem__("exit", 1)); rc, out = run(self.root); self.assertEqual(rc, 1)
    def test_harness_error_marker_fails(self):
        open(os.path.join(self.root, "steady", "HARNESS_ERROR.txt"), "w").write("KeyError(0)\n"); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("KeyError", out)
    def test_zero_decisions_fails(self):
        self.edit("hostile/hostile.json", lambda d: d["counts"]["shield_1proof/proof"].__setitem__("decisions", 0)); rc, out = run(self.root); self.assertEqual(rc, 1); self.assertIn("no decisions", out)

if __name__ == "__main__": unittest.main()
