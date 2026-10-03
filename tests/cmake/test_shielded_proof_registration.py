#!/usr/bin/env python3
"""Exercise the real CMake registration module in disposable source fixtures.

This checks registration/failure behavior, not executable test results. The
full Linux build and CTest run remain the execution gate.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
import copy

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "ci"))
from check_ctest_integrity import shielded_proof_inventory_errors

MODULE = Path(__file__).with_name("ShieldedProofTests.cmake").resolve()
REQUIRED = (
    "tests/zk/test_spartan_soundness.cpp",
    "tests/zk/test_compact_spartan.cpp",
    "src/consensus/shielded/compact_spartan_codec.cpp",
)


class RegistrationTest(unittest.TestCase):
    def configure(self, missing=None):
        temp = tempfile.TemporaryDirectory(prefix="dinero-proof-registration-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        source = root / "source"
        source.mkdir()
        for name in REQUIRED:
            if name != missing:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// Registration-only fixture; not a crypto test.\n")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(proof_registration LANGUAGES CXX)\n"
            "enable_testing()\n"
            "add_library(gtest INTERFACE)\n"
            "add_library(GTest::gtest ALIAS gtest)\n"
            "add_library(GTest::gtest_main INTERFACE IMPORTED)\n"
            "add_library(dinero_zk INTERFACE)\n"
            "add_library(dinero_crypto INTERFACE)\n"
            f'include("{MODULE.as_posix()}")\n'
            "dinero_register_shielded_proof_tests(${CMAKE_SOURCE_DIR})\n"
        )
        build = root / "build"
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build)],
                                text=True, capture_output=True, timeout=60)
        return result, build

    def test_both_suites_registered_without_cv_binding_source(self):
        result, build = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1",
                                 "-L", "shielded"], text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        tests = json.loads(result.stdout)["tests"]
        self.assertEqual({t["name"] for t in tests}, {"SpartanSoundness", "CompactSpartanCodec"})
        self.assertEqual(shielded_proof_inventory_errors(tests), [])
        for test in tests:
            props = {p["name"]: p["value"] for p in test["properties"]}
            self.assertIn("mandatory", props["LABELS"])
            self.assertIn("shielded", props["LABELS"])
            self.assertEqual(props["TIMEOUT"], 300)

    def test_each_missing_source_fails_configuration(self):
        for name in REQUIRED:
            with self.subTest(name=name):
                result, _ = self.configure(name)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Required shielded proof test source missing", result.stderr)
                self.assertIn(name, result.stderr)


class InventoryTest(unittest.TestCase):
    def test_missing_duplicate_disabled_and_unlabeled_fail(self):
        good = [{"name": name, "properties": [
            {"name": "LABELS", "value": ["shielded", "mandatory"]}]
        } for name in ("SpartanSoundness", "CompactSpartanCodec")]
        self.assertEqual(shielded_proof_inventory_errors(good), [])
        for index in (0, 1):
            with self.subTest(index=index):
                self.assertTrue(shielded_proof_inventory_errors(good[:index] + good[index+1:]))
                self.assertTrue(shielded_proof_inventory_errors(good + [good[index]]))
                disabled = copy.deepcopy(good)
                disabled[index]["properties"].append({"name": "DISABLED", "value": True})
                self.assertTrue(shielded_proof_inventory_errors(disabled))
                for missing_label in ("shielded", "mandatory"):
                    unlabeled = copy.deepcopy(good)
                    unlabeled[index]["properties"][0]["value"].remove(missing_label)
                    self.assertTrue(shielded_proof_inventory_errors(unlabeled))


if __name__ == "__main__":
    unittest.main()
