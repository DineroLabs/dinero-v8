import os
import pathlib
import stat
import subprocess
import tempfile
import time
import unittest


SCRIPT = (pathlib.Path(__file__).parents[1] / "deploy" /
          "dinero-height-watchdog-log-only")


class TestLegacyWatchdogLogOnly(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = pathlib.Path(self.tmp.name)
        self.state = root / "state"
        self.calls = root / "calls"
        self.height = root / "height"
        self.height.write_text("100\n", encoding="utf-8")

        self.systemctl = self._executable(
            "systemctl", '#!/bin/sh\nprintf "systemctl %s\\n" "$*" >> "$CALLS"\nexit 0\n')
        self.cli = self._executable(
            "dinero-cli", '#!/bin/sh\ncat "$HEIGHT_FILE"\n')
        self.logger = self._executable(
            "logger", '#!/bin/sh\nprintf "logger %s\\n" "$*" >> "$CALLS"\n')

    def _executable(self, name, body):
        path = pathlib.Path(self.tmp.name) / name
        path.write_text(body, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _run(self, stall_seconds="900"):
        env = {
            **os.environ,
            "STATE": str(self.state),
            "STALL_SECONDS": stall_seconds,
            "SYSTEMCTL": str(self.systemctl),
            "DINERO_CLI": str(self.cli),
            "LOGGER": str(self.logger),
            "CALLS": str(self.calls),
            "HEIGHT_FILE": str(self.height),
        }
        return subprocess.run([str(SCRIPT)], env=env, check=False,
                              text=True, capture_output=True)

    def test_expired_window_logs_but_never_restarts(self):
        self.state.write_text(f"100 {int(time.time()) - 1000}\n", encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text(encoding="utf-8")
        self.assertIn("observation only", calls)
        self.assertNotIn("restart", calls.replace("no restart", ""))

    def test_height_progress_resets_without_logging(self):
        self.state.write_text(f"99 {int(time.time()) - 1000}\n", encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.state.read_text(encoding="utf-8").split()[0], "100")
        calls = self.calls.read_text(encoding="utf-8")
        self.assertNotIn("logger ", calls)

    def test_unavailable_rpc_does_not_modify_state(self):
        self.cli.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        before = f"100 {int(time.time()) - 1000}\n"
        self.state.write_text(before, encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.state.read_text(encoding="utf-8"), before)


if __name__ == "__main__":
    unittest.main()
