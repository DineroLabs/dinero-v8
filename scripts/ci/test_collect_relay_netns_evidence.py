#!/usr/bin/env python3
import os
from pathlib import Path
import tempfile
import unittest

from collect_relay_netns_evidence import collect


class EvidenceCollection(unittest.TestCase):
    def test_preserves_failed_case_logs_without_entering_private_datadirs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "raw"
            nodes = source / "DEFAULT/nodes"
            private = nodes / "origin"
            private.mkdir(parents=True)
            (private / ".cookie").write_text("secret")
            (private / "wallet.log").write_text("private wallet log")
            (source / "summary.txt").write_text("DEFAULT exit=1")
            (source / "DEFAULT/test.log").write_text("handshake failed")
            (nodes / "origin.log").write_text("daemon stdout")
            private.chmod(0)
            try:
                output = root / "upload"
                self.assertEqual(collect(source, output, os.getuid(), os.getgid()), 3)
                self.assertEqual((output / "DEFAULT/nodes/origin.log").read_text(), "daemon stdout")
                self.assertEqual((output / "summary.txt").read_text(), "DEFAULT exit=1")
                self.assertFalse((output / "DEFAULT/nodes/origin").exists())
                self.assertEqual(private.stat().st_mode & 0o777, 0)
            finally:
                private.chmod(0o700)

    def test_refuses_symlinks_to_private_files_or_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "raw"
            source.mkdir()
            secret = root / "secret"
            secret.write_text("not an artifact")
            (source / "summary.txt").symlink_to(secret)
            (source / "DEFAULT").symlink_to(root, target_is_directory=True)
            (root / "test.log").write_text("also private")
            self.assertEqual(collect(source, root / "upload", os.getuid(), os.getgid()), 0)
            self.assertEqual(list((root / "upload").iterdir()), [])

    def test_missing_evidence_does_not_prevent_other_job_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(collect(root / "absent", root / "upload", os.getuid(), os.getgid()), 0)


if __name__ == "__main__":
    unittest.main()
