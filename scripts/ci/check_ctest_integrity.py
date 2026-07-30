#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""CTest integrity gate: every registered test must be runnable.

A test registered with add_test() whose executable was never built does not
fail a filtered CI ctest run — it either reports 'Not Run' (when invoked) or
simply never appears (when label-filtered away), and the repository silently
advertises coverage that no longer exists. RelayTlsKeypair rotted to
non-compiling for exactly this reason (see PR #416).

This script asks ctest for every registered test in a build directory
(`ctest --show-only=json-v1`) and verifies each test's command begins with
something executable: an existing file, or a name resolvable on PATH (e.g.
python3). Any miss is listed and the script exits nonzero.

Usage: check_ctest_integrity.py <build-dir>

Scope note: this gate can only see tests that ARE registered under the
build's configuration. A feature flag that is OFF in CI (e.g.
DINERO_ENABLE_QUIC) removes its tests from registration entirely — that
blindness is a coverage-policy decision, not something this script can
detect. It is documented here so nobody mistakes a green gate for proof
that every test in the tree compiles.
"""

import json
import os
import shutil
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    build_dir = sys.argv[1]
    if not os.path.isdir(build_dir):
        print(f"error: build dir not found: {build_dir}", file=sys.stderr)
        return 2

    proc = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        print("error: ctest --show-only=json-v1 failed:", file=sys.stderr)
        sys.stderr.write(proc.stderr)
        return 2

    model = json.loads(proc.stdout)
    tests = model.get("tests", [])
    if not tests:
        # An empty registry is its own failure mode: a configure change that
        # silently drops every test must not read as "all green".
        print("error: ctest reports ZERO registered tests — refusing to pass",
              file=sys.stderr)
        return 1

    missing = []
    for test in tests:
        name = test.get("name", "<unnamed>")
        command = test.get("command") or []
        if not command:
            # Registered but commandless (e.g. NOT_AVAILABLE placeholder from
            # a generator expression for a target that will not be built).
            missing.append((name, "<no command — target not built?>"))
            continue
        exe = command[0]
        if os.path.isabs(exe) or os.sep in exe:
            ok = os.path.isfile(exe) and os.access(exe, os.X_OK)
        else:
            ok = shutil.which(exe) is not None
        if not ok:
            missing.append((name, exe))

    if missing:
        print(f"CTEST INTEGRITY FAILURE: {len(missing)} of {len(tests)} "
              "registered tests have no runnable executable:", file=sys.stderr)
        for name, exe in missing:
            print(f"  {name}: {exe}", file=sys.stderr)
        print("\nEvery add_test() registration must correspond to a built "
              "executable. Either build the target in this pipeline or "
              "remove/condition the registration.", file=sys.stderr)
        return 1

    print(f"ctest integrity OK: {len(tests)} registered tests, "
          "all executables present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
