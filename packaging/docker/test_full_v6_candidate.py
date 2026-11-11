"""Focused checks for the prepared, unpublished full-v6 Docker image."""

import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from check_recovery_manifests import verify


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


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
        result = subprocess.run(["sh", str(ROOT / "docker-entrypoint.sh"),
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
                result = subprocess.run(["sh", str(ROOT / "docker-entrypoint.sh")],
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

    def test_live_image_uses_reviewed_full_v6_sources(self):
        self.assertEqual((ROOT / "Dockerfile").read_bytes(),
                         (HERE / "Dockerfile.full-v6").read_bytes())
        self.assertEqual((ROOT / "docker-entrypoint.sh").read_bytes(),
                         (HERE / "entrypoint-full-v6.sh").read_bytes())
        workflow = (ROOT / ".github/workflows/docker-publish.yml").read_text()
        self.assertIn('packaging/docker/check_image_layout.sh', workflow)
        self.assertIn('packaging/docker/test_full_v6_candidate.py', workflow)
        self.assertIn("steps.v.outputs.dry_run == 'false'", workflow)

    def test_workflow_docker_c_argument_executes_every_layout_check(self):
        workflow = (ROOT / ".github/workflows/docker-publish.yml").read_text()
        match = re.search(r'^\s+(docker run --rm --entrypoint /bin/sh dinerod:ci -c "\$\(cat packaging/docker/check_image_layout\.sh\)")$',
                          workflow, re.MULTILINE)
        self.assertIsNotNone(match, 'workflow must pass one complete script to docker -c')
        docker = self.root / 'docker'
        docker.write_text('#!/bin/sh\nprintf "%s\\0" "$@" > "$DOCKER_ARGV_FILE"\n')
        docker.chmod(0o755)
        captured = self.root / 'docker-argv'
        env = os.environ | {'DOCKER_ARGV_FILE': str(captured),
                            'PATH': str(self.root) + os.pathsep + os.environ['PATH']}
        result = subprocess.run(['/bin/sh', '-c', match.group(1)], cwd=ROOT,
                                env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = [item.decode() for item in captured.read_bytes().split(b'\0') if item]
        self.assertEqual(argv[:-1], ['run', '--rm', '--entrypoint', '/bin/sh', 'dinerod:ci', '-c'])
        script = (HERE / 'check_image_layout.sh').read_text()
        self.assertEqual(argv[-1], script.rstrip('\n'))

        primary = self.root / 'primary with spaces.dat'
        primary.touch()
        primary_sidecars = [Path(str(primary) + suffix) for suffix in
                            ('.manifest.json', '.publisher.manifest.json',
                             '.publisher.manifest.sig', '.publisher.receipt.json')]
        for path in primary_sidecars:
            path.touch()
        primary_sidecars[-1].write_text('{"publisher_signature_verified": true}\n')
        fallbacks = [self.root / f'fallback {i}.dat' for i in range(4)]
        for path in fallbacks:
            path.touch()
            Path(str(path) + '.manifest.json').touch()
        check_env = os.environ | {'DINERO_SNAPSHOT': str(primary),
                                  'DINERO_SNAPSHOT_FALLBACKS': ';'.join(map(str, fallbacks))}

        def run_check():
            return subprocess.run(['/bin/sh', '-c', argv[-1]], env=check_env,
                                  text=True, capture_output=True)

        self.assertEqual(run_check().returncode, 0)
        for path in [*primary_sidecars, *(p for fallback in fallbacks
                                          for p in (fallback, Path(str(fallback) + '.manifest.json')))]:
            with self.subTest(missing=path.name):
                data = path.read_bytes()
                path.unlink()
                self.assertNotEqual(run_check().returncode, 0)
                path.write_bytes(data)
        primary_sidecars[-1].write_text('{"publisher_signature_verified": false}\n')
        self.assertNotEqual(run_check().returncode, 0)
        primary_sidecars[-1].write_text('{"publisher_signature_verified": true}\n')
        check_env['DINERO_SNAPSHOT_FALLBACKS'] = ';'.join(map(str, fallbacks[:3]))
        self.assertNotEqual(run_check().returncode, 0)


if __name__ == "__main__":
    unittest.main()
