"""Exact pre-Orchard v5 capture against a fake selected-chain RPC."""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from snapshot_transport import inspect


SCRIPT = Path(__file__).with_name("capture_boundary_v5.py")
HEIGHT = 149999
BLOCK = "ab" * 32


class BoundaryCapture(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.datadir = self.root / "data"
        self.datadir.mkdir()
        self.cli = self.root / "cli"
        self.cli.write_text("""#!/usr/bin/env python3
import json,os,pathlib,shutil,sys
r=pathlib.Path(os.environ['FIXTURE_ROOT'])
args=sys.argv[2:]
if args[0]=='getblockchaininfo':
    print(json.dumps({'chain':os.environ.get('CHAIN','mainnet')}))
elif args[0]=='getblockcount':
    calls=r/'count-calls'
    n=int(calls.read_text()) if calls.exists() else 0
    calls.write_text(str(n+1))
    print(149998 if os.environ.get('WATCH_SEQUENCE') and n==0 else int(os.environ.get('TIP','149999')))
elif args[0]=='getblockhash':
    print(json.dumps('cd'*32 if os.environ.get('REORG_AFTER_EXPORT') and (r/'exported').exists() else 'ab'*32))
elif args[0]=='dumptxoutset':
    shutil.copyfile(r/'template.dat',args[1])
    (r/'exported').touch()
    print(json.dumps({'base_height':int(os.environ.get('RPC_EXPORT_HEIGHT','149999')),'base_hash':'ab'*32}))
else: sys.exit(99)
""")
        self.cli.chmod(0o755)
        self.write_template()

    def write_template(self, version=5, height=HEIGHT):
        payload = b"UXTO" + struct.pack("<I", version) + bytes.fromhex(BLOCK)[::-1] + struct.pack("<I", height) + bytes(24)
        (self.root / "template.dat").write_bytes(payload)
        return payload

    def run_capture(self, *args, **env_changes):
        env = dict(os.environ, DATADIR=str(self.datadir), CLI_BIN=str(self.cli), FIXTURE_ROOT=str(self.root), SNAPSHOT_BOUNDARY_POLL_SECONDS="0.001")
        env.update({name: str(value) for name, value in env_changes.items()})
        return subprocess.run(["python3", str(SCRIPT), *args], env=env, capture_output=True, text=True, timeout=5)

    def test_exact_height_stages_v5_and_is_idempotent(self):
        payload = self.write_template()
        first = self.run_capture()
        self.assertEqual(first.returncode, 0, first.stderr)
        pending = self.datadir / "snapshots"
        self.assertEqual((pending / "pending.dat").read_bytes(), payload)
        self.assertEqual(inspect(pending / "pending.dat"), (5, HEIGHT, BLOCK, "legacy"))
        record = json.loads((pending / "pending.json").read_text())
        self.assertEqual(record["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertTrue(record["boundary_capture"])
        second = self.run_capture()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("already captured", second.stdout)

    def test_watch_reaches_exact_tip(self):
        result = self.run_capture("--watch", WATCH_SEQUENCE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "count-calls").read_text(), "3")

    def test_corrupted_existing_capture_is_not_accepted(self):
        self.assertEqual(self.run_capture().returncode, 0)
        pending = self.datadir / "snapshots/pending.dat"
        with pending.open("ab") as stream:
            stream.write(b"corruption")
        result = self.run_capture()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum differs", result.stderr)

    def test_missed_height_fails_without_export(self):
        result = self.run_capture("--watch", TIP=150000)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missed", result.stderr)
        self.assertFalse((self.root / "exported").exists())

    def test_non_mainnet_refused(self):
        result = self.run_capture(CHAIN="regtest")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires mainnet", result.stderr)
        self.assertFalse((self.root / "exported").exists())

    def test_export_race_to_v6_fails_without_pending(self):
        self.write_template(version=6, height=150000)
        result = self.run_capture()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.datadir / "snapshots/pending.json").exists())

    def test_reorg_during_export_fails_without_pending(self):
        result = self.run_capture(REORG_AFTER_EXPORT="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("left selected chain", result.stderr)
        self.assertFalse((self.datadir / "snapshots/pending.json").exists())

    def test_unrelated_pending_is_never_replaced(self):
        pending = self.datadir / "snapshots"
        pending.mkdir()
        original = b"other candidate"
        (pending / "pending.dat").write_bytes(original)
        (pending / "pending.json").write_text("{}")
        result = self.run_capture()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((pending / "pending.dat").read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
