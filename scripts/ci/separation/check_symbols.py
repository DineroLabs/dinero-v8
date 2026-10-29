#!/usr/bin/env python3
"""Forbidden-symbol checker for the K3/W3 separation gates.

usage: check_symbols.py --binary <path> --gate {K3,W3} [--nm <tool>]
       check_symbols.py --nm-output <file> --gate {K3,W3}

Runs `nm -C <binary>` (or reads saved `nm -C` text with --nm-output) and
prints one line per distinct (pattern, symbol) pair whose demangled name
contains a forbidden pattern:

  FORBIDDEN <pattern> <symbol>

Every symbol counts, defined or undefined: K3/W3 run on the K1/W1 forced-link
executables, so a defined node symbol means node code was linked in, and an
undefined one means something still asks for it.

Lines starting with '#' are informational. Exit 0 clean, 1 on any forbidden
symbol, 2 on bad input (missing binary, nm failure, no symbols parsed).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

_COMMON = ["rocksdb::", "dinero::ChainDB", "ChainstateService", "dinero::Mempool"]
GATES = {
    "K3": _COMMON + ["sqlite3_", "dinero::Address::"],
    "W3": _COMMON + ["dinero::BlockStorage", "HeaderSyncManager", "PeerManager",
                     "TxInclusionAnalyzer"],
}

# "<addr> <type> <name>" for defined symbols, "<spaces> <type> <name>" for
# undefined ones. The demangled name may itself contain spaces.
NM_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?[A-Za-z?\-]\s+(.+?)\s*$")


class InputError(Exception):
    pass


def nm_text(args):
    if args.nm_output:
        try:
            return Path(args.nm_output).read_text(errors="replace")
        except OSError as e:
            raise InputError("cannot read %s: %s" % (args.nm_output, e))
    if not Path(args.binary).is_file():
        raise InputError("no such binary: %s" % args.binary)
    try:
        r = subprocess.run([args.nm, "-C", args.binary], capture_output=True,
                           text=True, errors="replace")
    except OSError as e:
        raise InputError("cannot run %s: %s" % (args.nm, e))
    if r.returncode != 0:
        raise InputError("%s -C %s failed (%d): %s"
                         % (args.nm, args.binary, r.returncode, r.stderr.strip()))
    return r.stdout


def symbols(text):
    out = []
    for line in text.splitlines():
        m = NM_LINE.match(line)
        if m:
            out.append(m.group(1))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--binary")
    src.add_argument("--nm-output")
    ap.add_argument("--gate", required=True, choices=sorted(GATES))
    ap.add_argument("--nm", default="nm")
    args = ap.parse_args(argv)

    try:
        syms = symbols(nm_text(args))
        if not syms:
            raise InputError("no symbols parsed (stripped binary or empty nm output?)")
    except InputError as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2

    seen = set()
    hits = []
    for sym in syms:
        for pat in GATES[args.gate]:
            if pat in sym and (pat, sym) not in seen:
                seen.add((pat, sym))
                hits.append("FORBIDDEN %s %s" % (pat, sym))
    for h in hits:
        print(h)
    print("# %s %s: %d symbols scanned, %d forbidden"
          % (args.gate, args.binary or args.nm_output, len(syms), len(hits)))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
