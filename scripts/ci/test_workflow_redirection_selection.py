"""Output-redirection parser regressions; never launch a subprocess."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "gate", Path(__file__).with_name("check_mandatory_tests_execute.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class OutputRedirectionSelection(unittest.TestCase):
    def setUp(self):
        blocker = patch("subprocess.Popen", side_effect=AssertionError(
            "Parser regression may not launch subprocesses"))
        blocker.start()
        self.addCleanup(blocker.stop)

    def rows(self, body):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "workflow.yml"
            path.write_text("jobs:\n  test:\n    steps:\n      - run: |\n" +
                            "".join("          " + line + "\n"
                                    for line in body.splitlines()))
            return gate.ctest_blocks(str(path))

    def selected(self, body):
        rows = self.rows(body)
        self.assertEqual(len(rows), 1)
        parsed = gate.parse_block(*rows[0], "fixture")
        return gate.selected_by(parsed, [("Safe", []), ("Other", [])])

    def test_inventory_output_is_not_execution(self):
        self.assertEqual(self.selected(
            "ctest --test-dir real --show-only=json-v1 > inventory.json"), set())

    def test_stderr_duplication_and_pipeline(self):
        self.assertEqual(self.selected(
            "ctest --test-dir real -R '^Safe$' 2>&1 | tee log"), {"Safe"})

    def test_redirection_before_later_selection(self):
        self.assertEqual(self.selected(
            "ctest --test-dir real 2>errors -R '^Safe$' >> output"), {"Safe"})

    def test_combined_and_noclobber_output(self):
        for redirect in [">log", ">>log", ">|log", "&>log", "&>>log",
                         "2>&1", "2>&-", '2>"log with spaces"']:
            with self.subTest(redirect=redirect):
                self.assertEqual(self.selected(
                    "ctest --test-dir real " + redirect + " -R '^Safe$'"), {"Safe"})

    def test_quoted_operators_are_arguments(self):
        row = self.rows("ctest --test-dir real -R 'A>B|C&D' > 'log > file'")[0]
        self.assertEqual(row[1], ["ctest", "--test-dir", "real", "-R", "A>B|C&D"])

    def test_escaped_operators_are_arguments(self):
        row = self.rows(r"ctest --test-dir real -R A\>B\&C >log")[0]
        self.assertEqual(row[1][-1], "A>B&C")

    def test_numeric_quoted_argument_is_not_descriptor(self):
        row = self.rows("ctest --test-dir real -R '2'>log")[0]
        self.assertEqual(row[1][-1], "2")

    def test_command_separators_still_separate(self):
        rows = self.rows("ctest --test-dir one >a && ctest --test-dir two 2>&1; "
                         "ctest --test-dir three &>c & ctest --test-dir four")
        self.assertEqual([r[1][2] for r in rows], ["one", "two", "three", "four"])

    def test_missing_and_unsupported_targets_refuse(self):
        for redirect in [">", "> &", "2>&file", "2>>&1", "&>|file", "<input",
                         "> >(echo data)", '>"$(echo data)"', ">`echo data`"]:
            with self.subTest(redirect=redirect), self.assertRaises(SystemExit):
                self.selected("ctest --test-dir real " + redirect)

    def test_unknown_flags_after_redirect_still_refuse(self):
        with self.assertRaises(SystemExit):
            self.selected("ctest --test-dir real >log --unknown-selection")

    def test_dynamic_selector_after_redirect_still_refuses(self):
        with self.assertRaises(SystemExit):
            self.selected('ctest --test-dir real >log -R "$(cat selectors)"')

    def test_real_unshield_workflow_keeps_both_selections(self):
        path = Path(__file__).resolve().parents[2] / ".github/workflows/unshield-qualification.yml"
        rows = gate.ctest_blocks(str(path))
        parsed = [gate.parse_block(*row, str(path)) for row in rows]
        execution = [r for r in parsed if not r["show_only"]]
        self.assertEqual(len(execution), 2)
        self.assertEqual([r["test_dir"] for r in execution],
                         ["../baseline/build-unshield", "build-unshield"])
        self.assertEqual(gate.selected_by(execution[0], [("ShieldedValidation", []),
                                                        ("GenesisInvariants", [])]),
                         {"GenesisInvariants"})
        self.assertEqual(gate.selected_by(execution[1], [("ShieldedValidation", [])]),
                         {"ShieldedValidation"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
