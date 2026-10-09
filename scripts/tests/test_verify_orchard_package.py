import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("package_gate", Path(__file__).parents[1] / "verify-orchard-package.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

DAEMON = {"schema": 1, "component": "dinerod", "orchard_backend": True, "profile": [7, 1, 1, 1, 5]}
QT = {"schema": 1, "component": "dinero-qt", "orchard_ui": True}

class PackageReport(unittest.TestCase):
    def test_expected_pair(self):
        self.assertEqual(gate.validate_report(json.dumps(DAEMON), "dinerod"), DAEMON)
        self.assertEqual(gate.validate_report(json.dumps(QT), "dinero-qt"), QT)

    def test_backend_off(self):
        with self.assertRaises(ValueError):
            gate.validate_report(json.dumps(dict(DAEMON, orchard_backend=False, profile=None)), "dinerod")

    def test_ui_off(self):
        with self.assertRaises(ValueError):
            gate.validate_report(json.dumps(dict(QT, orchard_ui=False)), "dinero-qt")

    def test_each_profile_field(self):
        for i in range(5):
            value = dict(DAEMON, profile=list(DAEMON["profile"]))
            value["profile"][i] += 1
            with self.subTest(field=i), self.assertRaises(ValueError):
                gate.validate_report(json.dumps(value), "dinerod")

    def test_strict_types(self):
        for value in [dict(DAEMON, schema=True), dict(DAEMON, orchard_backend=1),
                      dict(DAEMON, profile=[7, True, 1, 1, 5]), dict(DAEMON, profile=[7, 1., 1, 1, 5])]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.validate_report(json.dumps(value), "dinerod")

    def test_duplicate_or_old_text(self):
        for text in ['{"schema":0,"schema":1}', 'Dinero v8.1.13\n', json.dumps(DAEMON) + '{}',
                     '[INFO] [AutoReg] Mining extras registration deferred (requires context from main.cpp)\n' + json.dumps(DAEMON)]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.validate_report(text, "dinerod")

    def test_component_and_extra_fields(self):
        for value in [QT, dict(DAEMON, activation=True), dict(DAEMON, schema=2)]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.validate_report(json.dumps(value), "dinerod")

    def test_safe_version_fallback(self):
        import subprocess
        # A temporary existing file stands in for the immutable binary; the
        # subprocess is intercepted, so this unit test opens no sockets.
        with patch.object(gate.subprocess, "run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, json.dumps(DAEMON), "")
            gate.inspect_binary(Path(__file__), "dinerod")
            self.assertEqual(run.call_args.args[0][1:], ["--orchard-build-info", "--version"])
            self.assertEqual(run.call_args.kwargs["timeout"], 10)
            self.assertTrue(run.call_args.kwargs["check"])

if __name__ == "__main__":
    unittest.main()
