#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-test for the binary-level reachability stack gate.

The gate exists because shipped binaries lost UPnP, NAT-PMP and QUIC without
anyone noticing: a missing library was only a CMake STATUS line. These cases
pin the two ways the gate itself could quietly pass a bad binary -- treating
an absent RPC field as present, and reporting success when the daemon never
answered -- and that the daemon it launches never outlives the check.

Runs without a build directory: fake dinerod/dinero-cli executables stand in
for the real ones.

    python3 scripts/ci/test_check_reachability_stack.py
"""

import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATE_PATH = os.path.join(HERE, "check_reachability_stack.py")
_spec = importlib.util.spec_from_file_location("gate", GATE_PATH)
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)

FULL = {"port_mapping": {"upnp_compiled": True, "natpmp_compiled": True},
        "quic_transport": {"ngtcp2_available": True, "crypto_available": True}}


def write_exe(directory, name, body):
    path = os.path.join(directory, name)
    with open(path, "w") as f:
        f.write("#!/usr/bin/env python3\n" + body)
    os.chmod(path, 0o755)
    return path


def run_gate(*args, timeout=60):
    return subprocess.run([sys.executable, GATE_PATH, *args],
                          capture_output=True, text=True, timeout=timeout)


class EvaluateTests(unittest.TestCase):
    def test_all_present(self):
        self.assertEqual(gate.evaluate(FULL), [])

    def test_each_feature_reported_by_name(self):
        for path, name in [(("port_mapping", "upnp_compiled"), "upnp"),
                           (("port_mapping", "natpmp_compiled"), "natpmp"),
                           (("quic_transport", "ngtcp2_available"), "quic"),
                           (("quic_transport", "crypto_available"), "quic_crypto")]:
            info = copy.deepcopy(FULL)
            info[path[0]][path[1]] = False
            self.assertEqual(gate.evaluate(info), [name])

    def test_absent_fields_count_as_missing(self):
        self.assertEqual(gate.evaluate({}),
                         ["upnp", "natpmp", "quic", "quic_crypto"])
        self.assertEqual(gate.evaluate({"port_mapping": FULL["port_mapping"]}),
                         ["quic", "quic_crypto"])


class CliTests(unittest.TestCase):
    def test_from_json_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = os.path.join(tmp, "good.json")
            bad = os.path.join(tmp, "bad.json")
            with open(good, "w") as f:
                json.dump(FULL, f)
            with open(bad, "w") as f:
                json.dump({}, f)
            self.assertEqual(run_gate("--from-json", good).returncode, 0)
            r = run_gate("--from-json", bad)
            self.assertEqual(r.returncode, 1)
            self.assertEqual(
                r.stdout.strip().splitlines()[-1],
                "reachability stack incomplete: missing upnp, natpmp, quic, quic_crypto")

    def test_daemon_that_never_answers_exits_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            dinerod = write_exe(tmp, "dinerod", "import sys\nsys.exit(1)\n")
            cli = write_exe(tmp, "dinero-cli", "import sys\nsys.exit(1)\n")
            start = time.monotonic()
            r = run_gate("--dinerod", dinerod, "--cli", cli, "--timeout", "5")
            self.assertEqual(r.returncode, 2)
            self.assertLess(time.monotonic() - start, 15)

    def test_daemon_is_stopped_after_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            pidfile = os.path.join(tmp, "pid")
            dinerod = write_exe(
                tmp, "dinerod",
                "import os, time\n"
                "open(%r, 'w').write(str(os.getpid()))\n"
                "time.sleep(600)\n" % pidfile)
            cli = write_exe(tmp, "dinero-cli",
                            "import json\nprint(json.dumps(%r))\n" % (FULL,))
            r = run_gate("--dinerod", dinerod, "--cli", cli, "--timeout", "20")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            with open(pidfile) as f:
                pid = int(f.read())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
