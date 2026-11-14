#!/usr/bin/env python3
"""Keep the standard Intel and Ventura release filenames disjoint."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging/mac/sign-notarize-release.sh"
SOURCE = "a" * 40


def run_script(script: pathlib.Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["bash", str(script), "--version", "8.2.3", *args],
                          text=True, capture_output=True, check=False)


class MacReleaseAssetNames(unittest.TestCase):
    def test_distinct_standard_and_ventura_outputs(self) -> None:
        arm = run_script(SCRIPT, "--arch", "arm64", "--print-artifact-names")
        intel = run_script(SCRIPT, "--arch", "x86_64", "--print-artifact-names")
        ventura = run_script(SCRIPT, "--arch", "x86_64", "--variant",
                             "ventura-qt6.5", "--print-artifact-names")
        for result in (arm, intel, ventura):
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(result.stdout.splitlines()), 5)
        self.assertEqual(ventura.stdout.splitlines(), [
            "Dinero-v8.2.3-ventura-qt6.5-macOS-x86_64-qt.zip",
            "Dinero-v8.2.3-ventura-qt6.5-macOS-x86_64.dmg",
            "dinero-operator-v8.2.3-ventura-macOS-x86_64.tar.gz",
            "SHA256SUMS-macos-x86_64-desktop-8.2.3-ventura-qt6.5",
            "SHA256SUMS-macos-x86_64-8.2.3-ventura",
        ])
        self.assertEqual(intel.stdout.splitlines(), [
            "Dinero-v8.2.3-macOS-x86_64-qt.zip",
            "Dinero-v8.2.3-macOS-x86_64.dmg",
            "dinero-operator-v8.2.3-macOS-x86_64.tar.gz",
            "SHA256SUMS-macos-x86_64-desktop-8.2.3",
            "SHA256SUMS-macos-x86_64-8.2.3",
        ])
        all_names = arm.stdout.splitlines() + intel.stdout.splitlines() + ventura.stdout.splitlines()
        self.assertEqual(len(all_names), len(set(all_names)))

    def test_variant_requires_intel_and_known_name(self) -> None:
        arm = run_script(SCRIPT, "--arch", "arm64", "--variant",
                         "ventura-qt6.5", "--print-artifact-names")
        unknown = run_script(SCRIPT, "--arch", "x86_64", "--variant",
                             "ventura", "--print-artifact-names")
        self.assertEqual(arm.returncode, 2)
        self.assertIn("requires x86_64", arm.stderr)
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("unsupported macOS release variant", unknown.stderr)

    def test_ventura_stage_identity_checked_before_signing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            package_dir = temp / "packaging/mac"
            package_dir.mkdir(parents=True)
            script = package_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (package_dir / "dist/stage/dinero-qt.app").mkdir(parents=True)
            guard = package_dir / "assert-v8-release-lane.sh"
            guard.write_text("#!/bin/sh\necho REACHED_SIGNING_GUARD >&2\nexit 73\n")
            guard.chmod(0o755)
            args = ("--arch", "x86_64", "--variant", "ventura-qt6.5",
                    "--notary-profile", "test-only", "--expected-source-head", SOURCE)
            missing = run_script(script, *args)
            self.assertEqual(missing.returncode, 1)
            self.assertIn("Ventura stage provenance is missing", missing.stderr)

            receipt = package_dir / "dist/desktop-release-provenance-macos-x86_64-ventura-qt6.5.txt"
            receipt.write_text(f"variant=x86_64-ventura-qt6.5\narchitecture=x86_64\n"
                               f"qt_version=6.9.1\nsource_commit={SOURCE}\n")
            wrong_qt = run_script(script, *args)
            self.assertEqual(wrong_qt.returncode, 1)
            self.assertIn("qt_version=6.5.3", wrong_qt.stderr)

            receipt.write_text(f"variant=x86_64-ventura-qt6.5\narchitecture=x86_64\n"
                               f"qt_version=6.5.3\nsource_commit={'b' * 40}\n")
            wrong_source = run_script(script, *args)
            self.assertEqual(wrong_source.returncode, 1)
            self.assertIn("source does not match", wrong_source.stderr)

            receipt.write_text(f"variant=x86_64-ventura-qt6.5\narchitecture=x86_64\n"
                               f"qt_version=6.5.3\nsource_commit={SOURCE}\n")
            valid = run_script(script, *args)
            self.assertEqual(valid.returncode, 73, valid.stderr)
            self.assertIn("REACHED_SIGNING_GUARD", valid.stderr)


if __name__ == "__main__":
    unittest.main()
