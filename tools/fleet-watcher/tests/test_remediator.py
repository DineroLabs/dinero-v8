import json
import os
import pathlib
import stat
import subprocess
import tempfile
import time
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "remediator.py"
INCIDENT = "a" * 32


class TestRootRemediator(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = pathlib.Path(self.tmp.name)
        self.request = root / "request.json"
        self.receipts = root / "receipts"
        self.calls = root / "calls"
        self.systemctl = root / "systemctl"
        self.systemctl.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >> "$CALLS"\nexit "${SYSTEMCTL_RC:-0}"\n',
            encoding="utf-8")
        self.systemctl.chmod(self.systemctl.stat().st_mode | stat.S_IXUSR)

    def _write(self, **updates):
        payload = {
            "version": 1, "incident_id": INCIDENT, "node": "local",
            "rule": "node_behind", "requested_at": time.time(),
        }
        payload.update(updates)
        self.request.write_text(json.dumps(payload), encoding="utf-8")

    def _run(self, rc="0"):
        env = {**os.environ, "CALLS": str(self.calls), "SYSTEMCTL_RC": rc}
        return subprocess.run([
            str(SCRIPT), "--request", str(self.request),
            "--receipts", str(self.receipts), "--local-node", "local",
            "--service", "dinero.service", "--systemctl", str(self.systemctl),
        ], env=env, check=False, text=True, capture_output=True)

    def test_valid_request_restarts_exactly_once(self):
        self._write()
        first = self._run()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(self.calls.read_text(encoding="utf-8"),
                         "restart dinero.service\n")
        self.assertTrue((self.receipts / INCIDENT).exists())
        self.assertFalse(self.request.exists())

        self._write()
        second = self._run()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.calls.read_text(encoding="utf-8"),
                         "restart dinero.service\n")

    def test_wrong_node_is_rejected_without_restart(self):
        self._write(node="other")
        result = self._run()
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.request.exists())

    def test_stale_request_is_rejected_without_restart(self):
        self._write(requested_at=time.time() - 301)
        result = self._run()
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.request.exists())

    def test_failed_restart_is_not_retried_into_a_loop(self):
        self._write()
        failed = self._run(rc="1")
        self.assertEqual(failed.returncode, 1)
        self.assertTrue((self.receipts / INCIDENT).exists())
        self._write()
        retry = self._run()
        self.assertEqual(retry.returncode, 0)
        self.assertEqual(self.calls.read_text(encoding="utf-8").count("restart"), 1)


if __name__ == "__main__":
    unittest.main()
