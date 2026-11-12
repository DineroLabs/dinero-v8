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


def workflow_run_script(workflow: str, step_name: str) -> str:
    lines = workflow.splitlines()
    start = lines.index('      - name: ' + step_name)
    run = next(i for i in range(start + 1, len(lines)) if lines[i] == '        run: |')
    body = []
    for line in lines[run + 1:]:
        if line.startswith('      - ') or line.startswith('      #'):
            break
        body.append(line[10:] if line.startswith('          ') else '')
    return '\n'.join(body).rstrip() + '\n'


class FullV6DockerCandidateTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_fleet_publisher_key_is_in_source_and_pinned(self):
        relative = Path("packaging/snapshot/fleet-publisher-ed25519.pub.pem")
        key = ROOT / relative
        self.assertTrue(key.is_file(), "clean release checkout lacks the publisher key")
        self.assertEqual(hashlib.sha256(key.read_bytes()).hexdigest(),
                         "993b978c9be80a67ab4bc47be47ce6c4e21f9ce635757ddff8b6eed3fb4ec385")
        tracked = subprocess.run(["git", "ls-files", "--error-unmatch", "--", str(relative)],
                                 cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(tracked.returncode, 0,
                         "publisher key exists locally but is absent from the source checkout")

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
        self.assertLess(workflow.index('Real daemon fresh-volume and restart smoke'),
                        workflow.index('      - name: Build and push'))
        self.assertIn("- name: PR result is contract-only, not image-qualified", workflow)
        self.assertIn("- name: Build and load\n        if: github.event_name != 'pull_request'", workflow)
        self.assertIn("- name: Admit exact full-v6 catalog primary\n        if: github.event_name != 'pull_request'", workflow)

    def test_compiled_daemon_identity_is_checked_separately_from_archive_receipt(self):
        expected_sha = 'a' * 40
        stub = self.root / 'dinerod'
        stub.write_text('#!/bin/sh\nprintf "dinerod 8.2.3\\ncomponent: dinerod\\ncommit: %s\\n" "$TEST_BINARY_SHA"\n')
        stub.chmod(0o755)
        checker = HERE / 'check_binary_source.sh'

        def run_check(binary_sha: str, version: str = '8.2.3'):
            return subprocess.run(['sh', str(checker), str(stub), expected_sha, version],
                                  env=os.environ | {'TEST_BINARY_SHA': binary_sha},
                                  text=True, capture_output=True)

        self.assertEqual(run_check(expected_sha).returncode, 0)
        self.assertNotEqual(run_check('b' * 40).returncode, 0)
        self.assertNotEqual(run_check(expected_sha, '8.2.2').returncode, 0)

    def test_image_admission_requires_qualified_catalog(self):
        workflow = (ROOT / '.github/workflows/docker-publish.yml').read_text()
        script = workflow_run_script(workflow, 'Admit exact full-v6 catalog primary')
        current = subprocess.run(['bash', '-e', '-c', script], cwd=ROOT,
                                 text=True, capture_output=True)
        self.assertEqual(current.returncode, 0, current.stderr)

        catalog_dir = self.root / 'packaging/snapshot'
        catalog_dir.mkdir(parents=True)
        catalog_dir.joinpath('desktop-inputs-candidate.json').write_text(json.dumps({
            'primary': {'filename': 'dinero-assumeutxo-148375-v5.dat',
                        'height': 148375, 'format_version': 5,
                        'bytes': 4096, 'sha256': 'a' * 64,
                        'block_hash': 'b' * 64}}))
        legacy = subprocess.run(['bash', '-e', '-c', script], cwd=self.root,
                                text=True, capture_output=True)
        self.assertNotEqual(legacy.returncode, 0)
        self.assertIn('native-qualified catalog pin is unavailable', legacy.stderr)

    def test_manual_candidate_requires_exact_source_hashes_and_dry_run(self):
        workflow = (ROOT / '.github/workflows/docker-publish.yml').read_text()
        script = workflow_run_script(workflow, 'Resolve version')
        source_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT,
                                             text=True).strip()
        output = self.root / 'github-output'
        base = os.environ | {
            'GITHUB_OUTPUT': str(output), 'GITHUB_EVENT_NAME': 'workflow_dispatch',
            'GITHUB_REF_NAME': 'codex/v823-desktop-full-v6-packaging',
            'INPUT_VERSION': '8.2.3', 'INPUT_CANDIDATE_SOURCE': source_sha,
            'INPUT_PUBLISH_LATEST': 'false', 'INPUT_DRY_RUN': 'true',
            'RELEASE_PRERELEASE': 'false', 'RELEASE_DRAFT': 'false',
            'INPUT_SNAPSHOT_PAYLOAD_URL': 'https://example.org/snapshot.dat',
            'INPUT_SNAPSHOT_MANIFEST_URL': 'https://example.org/manifest.json',
            'INPUT_SNAPSHOT_SIGNATURE_URL': 'https://example.org/manifest.sig',
            'INPUT_BINARY_TARBALL_URL': 'https://example.org/dinero-linux-x86_64-8.2.3.tar.gz',
            'INPUT_BINARY_SUMS_URL': 'https://example.org/SHA256SUMS-linux-x86_64-8.2.3',
            'INPUT_BINARY_TARBALL_SHA256': 'a' * 64,
            'INPUT_BINARY_SUMS_SHA256': 'b' * 64,
        }

        def run_case(**changes):
            output.write_text('')
            return subprocess.run(['bash', '-e', '-o', 'pipefail', '-c', script],
                                  cwd=ROOT, env=base | changes, text=True,
                                  capture_output=True)

        result = run_case()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('source_sha=' + source_sha, output.read_text())
        for changes in (
            {'INPUT_CANDIDATE_SOURCE': 'c' * 40},
            {'INPUT_BINARY_TARBALL_SHA256': ''},
            {'INPUT_DRY_RUN': 'false'},
            {'INPUT_SNAPSHOT_SIGNATURE_URL': 'http://example.org/manifest.sig'},
            {'INPUT_BINARY_SUMS_URL': 'https://example.org/sums\nINJECT=1'},
        ):
            with self.subTest(changes=changes):
                self.assertNotEqual(run_case(**changes).returncode, 0)
        release = run_case(GITHUB_EVENT_NAME='release', GITHUB_REF_NAME='v8.2.3',
                           INPUT_CANDIDATE_SOURCE='', INPUT_SNAPSHOT_PAYLOAD_URL='',
                           INPUT_SNAPSHOT_MANIFEST_URL='', INPUT_SNAPSHOT_SIGNATURE_URL='',
                           INPUT_BINARY_TARBALL_URL='', INPUT_BINARY_SUMS_URL='',
                           INPUT_BINARY_TARBALL_SHA256='', INPUT_BINARY_SUMS_SHA256='',
                           INPUT_VERSION='', INPUT_DRY_RUN='')
        self.assertEqual(release.returncode, 0, release.stderr)

    def test_fresh_volume_smoke_restarts_same_volume_and_requires_armed_snapshot(self):
        docker = self.root / 'docker'
        docker.write_text('''#!/bin/sh
printf '%s\\n' "$*" >> "$DOCKER_CALLS"
case "$1" in
  volume|stop|rm) exit 0 ;;
  run) echo container-id; exit 0 ;;
  inspect)
    case "$*" in
      *State.Running*) echo true ;;
      *State.ExitCode*) echo "${DOCKER_STOP_EXIT_CODE:-0}" ;;
      *) exit 1 ;;
    esac
    exit 0 ;;
  exec) exit 0 ;;
  logs)
    if [ "$DOCKER_MISSING_SNAPSHOT_LOG" != 1 ]; then
      echo '[entrypoint] assumeutxo_snapshot=yes assumeutxo_fallbacks=yes assumeutxo_forward_connect=yes'
    fi
    exit 0 ;;
esac
exit 1
''')
        docker.chmod(0o755)
        calls = self.root / 'docker-calls'
        env = os.environ | {'DOCKER_CALLS': str(calls),
                            'DOCKER_MISSING_SNAPSHOT_LOG': '0',
                            'PATH': str(self.root) + os.pathsep + os.environ['PATH']}
        command = ['bash', str(HERE / 'smoke_fresh_restart.sh'), 'dinerod:ci']
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Docker fresh daemon reached RPC readiness', result.stdout)
        self.assertIn('Docker restart daemon reached RPC readiness', result.stdout)
        starts = [line for line in calls.read_text().splitlines() if line.startswith('run -d ')]
        self.assertEqual(len(starts), 2)
        self.assertEqual(starts[0].split('--mount ')[1].split(' ')[0],
                         starts[1].split('--mount ')[1].split(' ')[0])

        calls.write_text('')
        result = subprocess.run(command, env=env | {'DOCKER_MISSING_SNAPSHOT_LOG': '1'},
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('did not arm the primary and recovery snapshots', result.stderr)
        self.assertEqual(sum(line.startswith('run -d ') for line in calls.read_text().splitlines()), 1)

        calls.write_text('')
        result = subprocess.run(command, env=env | {'DOCKER_STOP_EXIT_CODE': '137'},
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('did not stop cleanly', result.stderr)
        self.assertEqual(sum(line.startswith('run -d ') for line in calls.read_text().splitlines()), 1)

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
