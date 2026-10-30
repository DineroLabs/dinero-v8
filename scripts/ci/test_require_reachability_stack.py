#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-test for the configure-time reachability stack gate.

DINERO_REQUIRE_REACHABILITY_STACK turns a missing UPnP/NAT-PMP library, a
QUIC-off build, or an OpenSSL without the QUIC TLS bridge into a configure
failure for release lanes, instead of the STATUS line that let shipped
binaries lose all three. These cases drive the gate in CMake script mode
with each feature state, so they need no build directory.

    python3 scripts/ci/test_require_reachability_stack.py
"""

import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DRIVER = os.path.join(ROOT, "tests", "cmake", "require_reachability_stack_driver.cmake")

ALL = dict(DINERO_ENABLE_PORTMAPPING="ON", DINERO_PORTMAP_HAVE_MINIUPNPC="ON",
           DINERO_PORTMAP_HAVE_NATPMP="ON", DINERO_ENABLE_QUIC="ON",
           DINERO_NGTCP2_CRYPTO_BACKEND="ossl")


def run(**defs):
    args = ["cmake"] + ["-D%s=%s" % kv for kv in defs.items()] + ["-P", DRIVER]
    r = subprocess.run(args, capture_output=True, text=True)
    # CMake re-wraps long messages; compare with whitespace collapsed.
    r.stderr = " ".join(r.stderr.split())
    return r


def without(**overrides):
    d = dict(ALL)
    for k, v in overrides.items():
        if v is None:
            d.pop(k, None)
        else:
            d[k] = v
    return d


class RequireReachabilityStackTests(unittest.TestCase):
    def test_required_and_complete_passes(self):
        r = run(DINERO_REQUIRE_REACHABILITY_STACK="ON", **ALL)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("missing", r.stderr)

    def test_required_reports_each_missing_by_name(self):
        cases = [
            (without(DINERO_PORTMAP_HAVE_NATPMP="OFF"), "missing: natpmp"),
            (without(DINERO_NGTCP2_CRYPTO_BACKEND="none"), "missing: quic_crypto"),
            (without(DINERO_ENABLE_QUIC="OFF", DINERO_NGTCP2_CRYPTO_BACKEND=None),
             "missing: quic, quic_crypto"),
        ]
        for defs, want in cases:
            r = run(DINERO_REQUIRE_REACHABILITY_STACK="ON", **defs)
            self.assertNotEqual(r.returncode, 0, want)
            self.assertIn(want, r.stderr)

    def test_required_all_missing_lists_in_order(self):
        r = run(DINERO_REQUIRE_REACHABILITY_STACK="ON")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing: upnp, natpmp, quic, quic_crypto", r.stderr)

    def test_not_required_only_warns(self):
        r = run(DINERO_REQUIRE_REACHABILITY_STACK="OFF")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Reachability stack incomplete (dev build)", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
