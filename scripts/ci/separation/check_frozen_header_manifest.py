#!/usr/bin/env python3
"""Gate K6, manifest half (spec 6.5): the CONSENSUS_MANIFEST entry in
include/consensus/freeze.h for include/consensus/tx_validation.h must be the
SHA-256 of the live file.

Only that entry is checked: the other manifest entries were already stale
before this project started and nothing verifies them at runtime (spec 6.6
finding); repairing them is not a separation task. The stored hash is of the
LF blob, so CRLF checkouts (Windows runners set core.autocrlf=true and
include/consensus/*.h carries no eol attribute) are normalized before
hashing.

Stdlib only. Exit 0 = entry matches; 1 = mismatch or no entry; 2 = input
error. No assert(): NDEBUG cannot disarm a gate, and neither may -O.
"""
import argparse
import hashlib
import os
import re
import sys

HEADER = "include/consensus/tx_validation.h"
MANIFEST = "include/consensus/freeze.h"


def lf(data):
    return data.replace(b"\r\n", b"\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--header", default=HEADER,
                    help="repo-relative frozen header (default %(default)s)")
    ap.add_argument("--manifest", default=MANIFEST,
                    help="repo-relative manifest header (default %(default)s)")
    args = ap.parse_args()

    try:
        with open(os.path.join(args.repo, args.header), "rb") as f:
            digest = hashlib.sha256(lf(f.read())).hexdigest()
        with open(os.path.join(args.repo, args.manifest), "rb") as f:
            manifest = lf(f.read()).decode("utf-8", "replace")
    except OSError as e:
        print(f"K6 input error: {e}", file=sys.stderr)
        return 2

    # The entry looks like {"include/consensus/tx_validation.h", "<64 hex>"}.
    pat = r'\{\s*"' + re.escape(args.header) + r'"\s*,\s*"([0-9a-f]{64})"\s*\}'
    m = re.search(pat, manifest)
    if not m:
        print(f"K6: no CONSENSUS_MANIFEST entry with a 64-hex-digit hash for "
              f"{args.header} in {args.manifest}")
        return 1
    if m.group(1) != digest:
        print(f"K6: CONSENSUS_MANIFEST stores {m.group(1)} for {args.header}, "
              f"but the file hashes to {digest}")
        return 1
    print(f"K6: CONSENSUS_MANIFEST entry for {args.header} matches "
          f"(sha256 {digest})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
