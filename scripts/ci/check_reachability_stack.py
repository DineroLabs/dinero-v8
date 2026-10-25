#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Release gate: prove a built dinerod carries the reachability stack.

A node that cannot open router ports (UPnP / NAT-PMP) or carry encrypted
relay traffic (QUIC + its TLS bridge) is invisible to the network, and every
one of those features used to disappear from a build silently: a missing
library was only a CMake STATUS line. This asks the finished binary itself.

It launches the daemon in regtest on a throwaway datadir, reads
`getnetworkinfo`, and checks the compiled-in flags. A field the binary does
not report counts as missing -- an absent answer is never a pass.

    check_reachability_stack.py --dinerod PATH --cli PATH [--timeout 90]
    check_reachability_stack.py --from-json FILE

Exit status: 0 all present, 1 something missing, 2 the daemon never answered.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

FEATURES = ("upnp", "natpmp", "quic", "quic_crypto")

# feature -> (getnetworkinfo object, field)
_FIELDS = {
    "upnp": ("port_mapping", "upnp_compiled"),
    "natpmp": ("port_mapping", "natpmp_compiled"),
    "quic": ("quic_transport", "ngtcp2_available"),
    "quic_crypto": ("quic_transport", "crypto_available"),
}


def evaluate(netinfo):
    """Return the features the binary lacks, in FEATURES order."""
    missing = []
    for feature in FEATURES:
        section, field = _FIELDS[feature]
        obj = netinfo.get(section)
        if not isinstance(obj, dict) or obj.get(field) is not True:
            missing.append(feature)
    return missing


def report(netinfo):
    missing = evaluate(netinfo)
    for feature in FEATURES:
        print("%s: %s" % (feature, "MISSING" if feature in missing else "compiled"))
    if missing:
        print("reachability stack incomplete: missing " + ", ".join(missing))
        return 1
    return 0


def _free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _tail(path, lines=40):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return ""


def probe(dinerod, cli, timeout):
    tmp = tempfile.mkdtemp(prefix="reachability-gate-")
    log_path = os.path.join(tmp, "dinerod.log")
    p2p, rpc = _free_port(), _free_port()
    log = open(log_path, "w")
    daemon = subprocess.Popen(
        [dinerod, "--regtest", "--datadir=%s" % tmp, "--port=%d" % p2p,
         "--rpcport=%d" % rpc, "--dnsseed=0", "--fixedseeds=0", "--listen=0"],
        stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                r = subprocess.run(
                    [cli, "-regtest", "-datadir=%s" % tmp, "-rpcport=%d" % rpc,
                     "getnetworkinfo"],
                    capture_output=True, text=True, timeout=15)
                if r.returncode == 0:
                    try:
                        return report(json.loads(r.stdout))
                    except ValueError:
                        pass
            except (OSError, subprocess.TimeoutExpired):
                pass
            time.sleep(1)
        print("dinerod did not answer getnetworkinfo within %ds; last daemon output:"
              % timeout)
        log.flush()
        print(_tail(log_path))
        return 2
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=30)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dinerod")
    parser.add_argument("--cli")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--from-json")
    args = parser.parse_args(argv)
    if args.from_json:
        with open(args.from_json) as f:
            return report(json.load(f))
    if not (args.dinerod and args.cli):
        parser.error("--dinerod and --cli are required (or --from-json)")
    return probe(args.dinerod, args.cli, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
