#!/usr/bin/env python3
"""Real RocksDB subprocess crash/reopen gates; not daemon or seed-fleet gates."""

import hashlib
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest


HARNESS = Path(sys.argv.pop(1)).resolve()
FIXTURE = Path(sys.argv.pop(1)).resolve()


def fingerprint(root):
    result = {}
    for path in [root, *sorted(root.rglob("*"))]:
        stat = path.stat()
        result[str(path.relative_to(root))] = (
            stat.st_ino, stat.st_mode, stat.st_size, stat.st_mtime_ns,
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None,
        )
    return result


class RetentionCopySubprocessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="retention_copy_subprocess_")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source"
        self.copy = self.root / "copy"
        self.execute([FIXTURE, "create", self.source])
        self.source_before = fingerprint(self.source)
        shutil.copytree(self.source, self.copy)
        self.args = [
            HARNESS, "--source-datadir", self.source, "--copy-datadir", self.copy,
            "--network", "testnet", "--recent-blocks", "6",
            "--historical-interval", "10", "--replay-per-step", "2",
            "--delete-per-batch", "3", "--pause-ms", "0",
        ]

    def execute(self, args, expected=0):
        result = subprocess.run(
            [str(arg) for arg in args], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=30, check=False,
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expected, output)
        return output

    def harness(self, *args, expected=0):
        output = self.execute([*self.args, *args], expected)
        self.assertEqual(fingerprint(self.source), self.source_before,
                         "source file bytes, inodes, modes, sizes, or mtimes changed")
        return output

    def verify(self, stage):
        output = self.execute([FIXTURE, "verify", self.copy, stage])
        self.assertIn("verified_heights=33", output)
        self.assertRegex(output, r"verified_proofs=[1-9][0-9]+")
        self.assertEqual(fingerprint(self.source), self.source_before)

    def test_real_lock_rejection_crash_reopen_and_resume_preserve_every_height_and_proof(self):
        audit = self.harness()
        self.assertIn("mode=audit", audit)
        self.assertIn("deleted_height_slots=0", audit)
        self.verify("original")

        # Hold the actual database lock in another process. The harness must
        # reject this open without deleting or unlinking the LOCK file.
        ready = self.root / "lock-ready"
        holder = subprocess.Popen(
            [str(FIXTURE), "hold-lock", str(self.copy), str(ready)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while not ready.exists() and time.monotonic() < deadline:
                if holder.poll() is not None:
                    output, error = holder.communicate()
                    self.fail("lock holder exited early: " + output + error)
                time.sleep(0.01)
            self.assertTrue(ready.exists(), "fixture never acquired the RocksDB lock")
            lock = self.copy / "blockchain" / "chaindb" / "LOCK"
            inode = lock.stat().st_ino
            rejected = self.harness("--apply", expected=2)
            self.assertIn("copy ChainDB open failed", rejected)
            self.assertIn("lock", rejected.lower())
            self.assertEqual(lock.stat().st_ino, inode)
        finally:
            if holder.poll() is None:
                holder.communicate("release\n", timeout=10)
            for stream in (holder.stdin, holder.stdout, holder.stderr):
                if stream and not stream.closed:
                    stream.close()
        self.assertEqual(holder.returncode, 0)
        self.verify("original")

        crashed = self.harness("--apply", "--crash-after-delete-batches", "1", expected=86)
        self.assertIn("event=injected_crash", crashed)
        self.assertIn("deleted_height_slots=3", crashed)
        self.assertIn("delete_batches=1", crashed)
        # New process, fresh RocksDB open, replay and proof generation at ALL
        # original heights immediately after the no-destructor _Exit(86).
        self.verify("partial")

        resumed = self.harness("--apply", "--compact")
        self.assertIn("event=compaction status=complete", resumed)
        self.assertIn("event=result status=complete", resumed)
        self.verify("pruned")

    def test_height_only_wallet_recovery_marker_protects_its_anchor(self):
        with sqlite3.connect(self.copy / "blockchain" / "utxo") as connection:
            connection.execute("DELETE FROM utxo_metadata")
            connection.execute("INSERT INTO utxo_metadata VALUES (?,?)",
                               ("wallet_snapshot_recovery_base_height", "13"))
        output = self.harness("--apply")
        self.assertIn('key="wallet_snapshot_recovery_base_height" height=13', output)
        self.assertIn('protected_heights="13"', output)
        self.verify("pruned")

    def test_height_only_wallet_recovery_marker_rejects_invalid_or_future_height(self):
        for height, reason in (("0", "invalid unsigned integer"),
                               ("13junk", "invalid unsigned integer"),
                               ("33", "snapshot base is above chain tip")):
            with self.subTest(height=height):
                with sqlite3.connect(self.copy / "blockchain" / "utxo") as connection:
                    connection.execute("DELETE FROM utxo_metadata")
                    connection.execute("INSERT INTO utxo_metadata VALUES (?,?)",
                                       ("wallet_snapshot_recovery_base_height", height))
                output = self.harness("--apply", expected=2)
                self.assertIn(reason, output)
                self.verify("original")

    def test_missing_delta_skips_whole_interval_and_refuses_requested_compaction(self):
        self.execute([FIXTURE, "remove-delta-eight", self.copy])
        self.assertEqual(fingerprint(self.source), self.source_before)
        output = self.harness("--apply", "--compact", expected=3)
        self.assertIn("event=skipped_interval interval_start=0 interval_end=10", output)
        self.assertIn("replay-missing-delta-sidecar-at-8", output)
        self.assertIn("event=result status=skipped_intervals exit_code=3", output)
        self.assertIn("compaction=not_performed", output)
        self.assertNotIn("event=compaction status=started", output)
        self.assertNotIn("event=compaction status=complete", output)
        # All 0..10 checkpoints remain; later valid intervals lose interiors.
        # Even height 8 remains provable using its retained exact checkpoint.
        self.verify("skipped")


if __name__ == "__main__":
    unittest.main()
