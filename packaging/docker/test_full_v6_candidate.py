"""Focused checks for the prepared, unpublished full-v6 Docker image."""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from check_recovery_manifests import verify


HERE = Path(__file__).resolve().parent


class FullV6DockerCandidateTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_entrypoint_passes_all_four_fallbacks_to_daemon(self):
        primary = self.root / "primary.dat"
        primary.touch()
        fallbacks = [self.root / f"fallback-{i}.dat" for i in range(4)]
        for path in fallbacks:
            path.touch()
        stub = self.root / "dinerod"
        stub.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGUMENTS_FILE"\n')
        stub.chmod(0o755)
        arguments = self.root / "arguments"
        env = os.environ | {
            "DINERO_DATADIR": str(self.root / "data"),
            "DINERO_SNAPSHOT": str(primary),
            "DINERO_SNAPSHOT_FALLBACKS": ";".join(map(str, fallbacks)),
            "ARGUMENTS_FILE": str(arguments),
            "PATH": str(self.root) + os.pathsep + os.environ["PATH"],
        }
        result = subprocess.run(["sh", str(HERE / "entrypoint-full-v6.sh"),
                                 "-rpcpassword=CANARY_SECRET"],
                                env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = arguments.read_text().splitlines()
        self.assertIn("--assumeutxo_snapshot=" + str(primary), argv)
        self.assertIn("--assumeutxo_forward_connect=1", argv)
        self.assertIn("--assumeutxo_snapshot_fallbacks=" + ";".join(map(str, fallbacks)), argv)
        self.assertNotIn("CANARY_SECRET", result.stdout + result.stderr)

    def test_missing_or_empty_fallback_refuses_start(self):
        primary = self.root / "primary.dat"
        primary.touch()
        present = self.root / "present.dat"
        present.touch()
        for fallback_list in (str(present) + ";" + str(self.root / "missing.dat"),
                              str(present) + ";"):
            with self.subTest(fallback_list=fallback_list):
                env = os.environ | {
                    "DINERO_DATADIR": str(self.root / "data"),
                    "DINERO_SNAPSHOT": str(primary),
                    "DINERO_SNAPSHOT_FALLBACKS": fallback_list,
                }
                result = subprocess.run(["sh", str(HERE / "entrypoint-full-v6.sh")],
                                        env=env, text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("fallback", result.stderr)

    def test_recovery_manifest_checker_requires_all_four_exact_payloads(self):
        names = ["dinero-assumeutxo-148375-v5.dat",
                 "dinero-assumeutxo-99677-v4.dat",
                 "dinero-assumeutxo-73035-v4.dat",
                 "utxo-snapshot-65300.dat"]
        heights = [148375, 99677, 73035, 65300]
        versions = [5, 4, 4, 4]
        pins = []
        for name, height, version in zip(names, heights, versions):
            block_hash = f"{height:064x}"
            header = bytearray(68)
            header[:4] = b"UXTO"
            struct.pack_into("<I", header, 4, version)
            header[8:40] = bytes.fromhex(block_hash)[::-1]
            struct.pack_into("<I", header, 40, height)
            payload = self.root / name
            payload.write_bytes(header)
            pin = {"filename": name, "height": height, "format_version": version,
                   "block_hash": block_hash, "bytes": len(header),
                   "sha256": hashlib.sha256(header).hexdigest()}
            pins.append(pin)
            manifest = {
                "network": "mainnet",
                "snapshot": {"snapshot_file": name,
                             **{key: pin[key] for key in ("height", "block_hash", "bytes", "sha256")}},
            }
            if version == 5:
                manifest["format_version"] = version
            (self.root / (name + ".manifest.json")).write_text(json.dumps(manifest))
        catalog = self.root / "catalog.json"
        catalog.write_text(json.dumps({"fallbacks": [pins[0], pins[1], pins[3]]}))
        self.assertEqual(verify(self.root, catalog, pins[2]), names)
        # A missing old recovery base fails image preparation, even though the
        # new primary may be present.
        (self.root / names[3]).unlink()
        with self.assertRaises(FileNotFoundError):
            verify(self.root, catalog, pins[2])


if __name__ == "__main__":
    unittest.main()
