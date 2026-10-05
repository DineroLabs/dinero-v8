#!/usr/bin/env python3
"""Policy accounting tests with synthetic inventories; never launch CTest."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import check_mandatory_tests_execute as gate
import yaml


EXCLUDED = sorted(gate.EXCLUDED_TESTS)
INVENTORY = [("Plain", []), ("Unrelated", ["integration"])] + [
    (name, ["integration"]) for name in EXCLUDED]
BROAD = "ctest --test-dir build-tests --label-exclude integration"


class ExecutionExclusions(unittest.TestCase):
    def invoke(self, commands=(), baseline="Unrelated\n", flags=(),
               inventory=None, shared=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflow = root / "workflow.yml"
            workflow.write_text("jobs:\n  check:\n    steps:\n      - run: |\n" +
                                "".join("          " + c + "\n"
                                        for c in (BROAD, *commands)))
            debt = root / "baseline.txt"
            debt.write_text(baseline)
            records = INVENTORY if inventory is None else inventory
            def discover(build):
                return shared if build == "primary" else records
            output = io.StringIO()
            with patch.object(gate, "registered_tests", side_effect=discover), \
                 patch.object(sys, "argv", ["gate", "build-tests", str(workflow),
                                            "--baseline", str(debt), *flags]), \
                 patch("subprocess.Popen", side_effect=AssertionError(
                     "policy tests must not launch any subprocess")), \
                 contextlib.redirect_stdout(output):
                status = gate.main()
            return status, output.getvalue(), debt.read_text()

    def test_explicitly_excluded_get_no_execution_credit(self):
        status, out, _ = self.invoke()
        self.assertEqual(status, 0)
        self.assertIn("selected here: 1", out)
        self.assertIn("unexecuted debt: 1", out)
        self.assertIn("policy excluded (NOT RUN): 2", out)
        for name in EXCLUDED:
            self.assertIn("EXCLUDED (NOT RUN): " + name, out)

    def test_both_exact_selections_fail(self):
        for name in EXCLUDED:
            with self.subTest(name=name):
                status, out, _ = self.invoke([
                    "ctest --test-dir build-tests -R '^" + name + "$'"])
                self.assertEqual(status, 1)
                self.assertIn(name, out)

    def test_broad_selection_fails(self):
        self.assertEqual(self.invoke(["ctest --test-dir build-tests"])[0], 1)

    def test_label_selection_fails(self):
        self.assertEqual(self.invoke([
            "ctest --test-dir build-tests -L integration"])[0], 1)

    def test_loop_selection_fails(self):
        self.assertEqual(self.invoke([
            "for t in " + " ".join(EXCLUDED) + "; do",
            'ctest --test-dir build-tests -R "^${t}$"', "done"])[0], 1)

    def test_other_build_selection_fails_without_credit(self):
        self.assertEqual(self.invoke([
            "ctest --test-dir other-build -R '^" + EXCLUDED[0] + "$'"])[0], 1)

    def test_show_only_is_not_execution(self):
        status, out, _ = self.invoke(["ctest --test-dir build-tests -N"])
        self.assertEqual(status, 0)
        self.assertIn("selected here: 1", out)

    def test_explain_cannot_bypass_exclusion(self):
        self.assertEqual(self.invoke(["ctest --test-dir build-tests"],
                                     flags=["--explain"])[0], 1)

    def test_update_cannot_bypass_exclusion_or_rewrite_baseline(self):
        status, _, debt = self.invoke(["ctest --test-dir build-tests"],
                                      flags=["--update"])
        self.assertEqual(status, 1)
        self.assertEqual(debt, "Unrelated\n")

    def test_secondary_scope_cannot_hide_shared_exclusion(self):
        records = INVENTORY + [("Unique", [])]
        status, _, _ = self.invoke(["ctest --test-dir build-tests"],
                                  inventory=records, shared=INVENTORY,
                                  flags=["--only-absent-from", "primary"])
        self.assertEqual(status, 1)

    def test_update_never_inserts_policy_into_debt(self):
        status, out, debt = self.invoke(flags=["--update"])
        self.assertEqual(status, 0)
        self.assertIn("Unrelated", debt)
        for name in EXCLUDED:
            self.assertNotIn(name, debt)
            self.assertIn("EXCLUDED (NOT RUN): " + name, out)

    def test_policy_in_debt_is_rejected(self):
        for flags in [[], ["--update"], ["--explain"]]:
            with self.subTest(flags=flags):
                status, out, _ = self.invoke(
                    baseline="Unrelated\n" + EXCLUDED[0], flags=flags)
                self.assertEqual(status, 1)
                self.assertIn("must not be hidden", out)

    def test_unrelated_new_dead_coverage_still_fails(self):
        status, out, _ = self.invoke(baseline="")
        self.assertEqual(status, 1)
        self.assertIn("Unrelated", out)

    def test_unrelated_revived_and_stale_entries_still_fail(self):
        for baseline in ["Unrelated\nPlain", "Unrelated\nGone"]:
            with self.subTest(baseline=baseline):
                self.assertEqual(self.invoke(baseline=baseline)[0], 1)

    def test_absent_exclusions_do_not_claim_coverage(self):
        status, out, _ = self.invoke(inventory=INVENTORY[:2])
        self.assertEqual(status, 0)
        self.assertIn("policy excluded (NOT RUN): 0", out)

    def test_similarly_named_test_is_not_excused(self):
        status, out, _ = self.invoke(inventory=INVENTORY + [
            (EXCLUDED[0] + "Other", ["integration"])])
        self.assertEqual(status, 1)
        self.assertIn(EXCLUDED[0] + "Other", out)

    def test_actual_generated_selector_guard_refuses_excluded_names(self):
        workflow = Path(__file__).resolve().parents[2] / \
            ".github/workflows/orchard-backend.yml"
        document = yaml.safe_load(workflow.read_text())
        blocks = [step["run"] for job in document["jobs"].values()
                  for step in job.get("steps", [])
                  if step.get("name") ==
                  "Build and execute real mempool runtime test consumers"]
        self.assertEqual(len(blocks), 1)
        block = blocks[0]
        begin = block.index("# Refuse excluded controls")
        end = block.index("json.dump({'tests':tests}", begin)
        guard = block[begin:end]
        for name in EXCLUDED:
            with self.subTest(name=name), patch("subprocess.Popen",
                    side_effect=AssertionError("No subprocess allowed")):
                with self.assertRaisesRegex(SystemExit, name):
                    exec(compile(guard, str(workflow), "exec"),
                         {"tests": [{"name": "Safe"}, {"name": name}]})
        records = [{"name": "Safe"}]
        with patch("subprocess.Popen", side_effect=AssertionError(
                "No subprocess allowed")):
            exec(compile(guard, str(workflow), "exec"), {"tests": records})
        self.assertEqual(records, [{"name": "Safe"}])


if __name__ == "__main__":
    unittest.main()
