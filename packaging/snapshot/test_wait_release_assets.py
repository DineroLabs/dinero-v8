"""Exercise tag-time release asset readiness without GitHub access."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "packaging/snapshot"))
import wait_release_assets as target


WORKFLOW = ROOT / ".github/workflows/windows-release.yml"
NAMES = target.REQUIRED


class WaitReleaseAssetsTests(unittest.TestCase):
    @staticmethod
    def uploaded(names):
        return [{"name": name, "size": 1, "state": "uploaded"} for name in names]

    def run_wait(self, sequence, *, timeout=0.5):
        calls = []

        def fake_gh(argv, **kwargs):
            calls.append((argv, kwargs))
            item = sequence[min(len(calls) - 1, len(sequence) - 1)]
            if item == "error":
                return subprocess.CompletedProcess(argv, 1, "", "release not found yet")
            return subprocess.CompletedProcess(argv, 0, json.dumps({"assets": item}), "")

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(target.subprocess, "run", side_effect=fake_gh):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                success = target.wait("v8.2.3", "DineroLabs/dinero-v8", timeout, 0.01)
        for argv, kwargs in calls:
            self.assertEqual(argv, ["gh", "release", "view", "v8.2.3", "--repo",
                                    "DineroLabs/dinero-v8", "--json", "assets"])
            self.assertTrue(kwargs["timeout"] > 0)
        return success, len(calls), stdout.getvalue(), stderr.getvalue()

    def test_waits_for_all_four_uploaded_nonempty_assets(self):
        not_uploaded = self.uploaded(NAMES[:3]) + [
            {"name": NAMES[3], "size": 1, "state": "new"}]
        empty = self.uploaded(NAMES[:3]) + [
            {"name": NAMES[3], "size": 0, "state": "uploaded"}]
        success, calls, stdout, stderr = self.run_wait([
            "error", not_uploaded, empty, self.uploaded(NAMES)])
        self.assertTrue(success, stderr)
        self.assertGreaterEqual(calls, 4)
        self.assertIn("four full-v6 release assets ready", stdout)

    def test_times_out_with_exact_missing_asset(self):
        success, calls, _, stderr = self.run_wait([
            self.uploaded(NAMES[:3])], timeout=0.04)
        self.assertFalse(success)
        self.assertGreaterEqual(calls, 1)
        self.assertIn(NAMES[3], stderr)

    def test_workflow_runs_test_and_waits_only_in_tag_path(self):
        source = WORKFLOW.read_text()
        self.assertIn("python -B packaging/snapshot/test_wait_release_assets.py -v", source)
        tag_path = source.index('if [ -n "$PRIMARY_RELEASE" ]; then')
        wait = source.index("python packaging/snapshot/wait_release_assets.py", tag_path)
        download = source.index('gh release download "$PRIMARY_RELEASE"', tag_path)
        candidate = source.index("else\n            python packaging/snapshot/fetch_desktop_primary.py", tag_path)
        self.assertLess(tag_path, wait)
        self.assertLess(wait, download)
        self.assertLess(download, candidate)


if __name__ == "__main__":
    unittest.main()
