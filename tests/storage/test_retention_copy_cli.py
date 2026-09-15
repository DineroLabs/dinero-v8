#!/usr/bin/env python3
"""CLI/path rejection tests; these do not claim real seed reclamation coverage."""

import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv.pop(1)).resolve()


class RetentionCopyCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="retention_copy_cli_")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source"
        self.copy = self.root / "copy"
        self.source.mkdir()
        (self.source / "source-sentinel").write_text("source must stay untouched\n")
        self.db = self.copy / "blockchain" / "chaindb"
        self.db.mkdir(parents=True)
        (self.db / "CURRENT").write_text("MANIFEST-000001\n")
        (self.db / "MANIFEST-000001").write_bytes(b"not a real RocksDB fixture")
        self.utxo = self.copy / "blockchain" / "utxo"
        with sqlite3.connect(self.utxo) as connection:
            connection.execute("CREATE TABLE utxo_metadata (key TEXT PRIMARY KEY, value TEXT)")
        self.args = [
            "--source-datadir", str(self.source), "--copy-datadir", str(self.copy),
            "--network", "testnet", "--pause-ms", "0",
        ]

    def invoke(self, args, expected=2, reason=None):
        result = subprocess.run(
            [str(BINARY), *args], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=10, check=False,
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expected, output)
        if reason is not None:
            self.assertIn(reason, output)
        self.assertEqual(
            (self.source / "source-sentinel").read_text(), "source must stay untouched\n"
        )
        return output

    def test_help_does_not_require_paths(self):
        self.invoke(["--help"], expected=0, reason="Audit is the default")

    def test_required_arguments_and_network(self):
        self.invoke([], reason="both --source-datadir and --copy-datadir are required")
        self.invoke(self.args[:4], reason="explicit valid --network is required")
        args = self.args.copy()
        args[5] = "unknown-chain"
        self.invoke(args, reason="explicit valid --network is required")

    def test_mutating_flags_require_apply(self):
        self.invoke([*self.args, "--compact"], reason="--compact requires --apply")
        self.invoke([*self.args, "--crash-after-delete-batches", "1"],
                    reason="--crash-after-delete-batches requires --apply")

    def test_numeric_bounds_unknown_options_and_duplicates(self):
        for option in ("--recent-blocks", "--historical-interval", "--replay-per-step",
                       "--delete-per-batch", "--crash-after-delete-batches"):
            for value in ("0", "-1", "+1", "1garbage", "4294967296"):
                with self.subTest(option=option, value=value):
                    self.invoke([*self.args, option, value], reason="invalid unsigned integer")
        self.invoke([*self.args, "--protect-height", "2147483648"],
                    reason="protected height exceeds ChainDB height range")
        self.invoke([*self.args[:-2], "--pause-ms", "60001"], reason="pause-ms exceeds")
        self.invoke([*self.args, "--apply", "--apply"], reason="duplicate option")
        self.invoke([*self.args, "--surprise"], reason="unknown option")

    def test_equal_or_alias_roots_are_rejected(self):
        args = self.args.copy()
        args[3] = str(self.source)
        self.invoke(args, reason="aliases or nested roots refused")
        alias = self.root / "source-alias"
        alias.symlink_to(self.source, target_is_directory=True)
        args[3] = str(alias)
        self.invoke(args, reason="aliases or nested roots refused")

    def test_nested_roots_are_rejected_in_both_directions(self):
        child = self.source / "nested-copy"
        child.mkdir()
        args = self.args.copy()
        args[3] = str(child)
        self.invoke(args, reason="aliases or nested roots refused")
        args = self.args.copy()
        args[1] = str(self.root)
        self.invoke(args, reason="aliases or nested roots refused")

    def test_copy_database_symlink_is_rejected(self):
        (self.db / "000007.sst").symlink_to(self.source / "source-sentinel")
        self.invoke(self.args, reason="copy symlink refused")

    def test_blockchain_directory_symlink_is_rejected(self):
        actual = self.root / "moved-blockchain"
        (self.copy / "blockchain").rename(actual)
        (self.copy / "blockchain").symlink_to(actual, target_is_directory=True)
        self.invoke(self.args, reason="copy symlink refused")

    def test_hardlinked_database_file_is_rejected(self):
        os.link(self.source / "source-sentinel", self.db / "000007.sst")
        self.invoke(self.args, reason="copy hardlink refused")

    def test_hardlinked_sqlite_is_rejected(self):
        os.link(self.utxo, self.root / "another-utxo-link")
        self.invoke(self.args, reason="copy hardlink refused")

    def test_missing_current_is_rejected_before_open(self):
        (self.db / "CURRENT").unlink()
        self.invoke(self.args, reason="CURRENT is missing")
        self.assertFalse((self.db / "LOCK").exists())

    def test_invalid_manifest_reference_is_rejected_before_open(self):
        for value in ("../../MANIFEST-1\n", "MANIFEST-999999\n", "OTHER-000001\n"):
            with self.subTest(value=value):
                (self.db / "CURRENT").write_text(value)
                self.invoke(self.args, reason="CURRENT does not name an existing MANIFEST")
                self.assertFalse((self.db / "LOCK").exists())

    def test_missing_sqlite_is_rejected_even_in_audit(self):
        self.utxo.unlink()
        self.invoke(self.args, reason="cannot lstat copy entry")
        self.assertFalse((self.db / "LOCK").exists())

    def test_sqlite_without_metadata_table_is_rejected_even_in_audit(self):
        with sqlite3.connect(self.utxo) as connection:
            connection.execute("DROP TABLE utxo_metadata")
        self.invoke(self.args, reason="copy UTXO metadata query failed")
        self.assertFalse((self.db / "LOCK").exists())

    def test_incomplete_snapshot_marker_is_rejected_before_chain_open(self):
        with sqlite3.connect(self.utxo) as connection:
            connection.execute("INSERT INTO utxo_metadata VALUES (?,?)",
                               ("assumeutxo_base_height", "13"))
        self.invoke(self.args, reason="incomplete copy UTXO snapshot metadata")
        self.assertFalse((self.db / "LOCK").exists())

    def test_invalid_snapshot_height_is_rejected_before_chain_open(self):
        for value in ("0", "-13", "13garbage", "4294967296"):
            with self.subTest(value=value), sqlite3.connect(self.utxo) as connection:
                connection.execute("INSERT OR REPLACE INTO utxo_metadata VALUES (?,?)",
                                   ("assumeutxo_base_height", value))
                connection.execute("INSERT OR REPLACE INTO utxo_metadata VALUES (?,?)",
                                   ("assumeutxo_base_block", "ab" * 32))
                connection.commit()
                self.invoke(self.args, reason="invalid unsigned integer")
                self.assertFalse((self.db / "LOCK").exists())


if __name__ == "__main__":
    unittest.main()
