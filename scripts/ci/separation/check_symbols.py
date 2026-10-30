#!/usr/bin/env python3
"""Forbidden-symbol checker for the K3/W3 separation gates.

usage: check_symbols.py --binary <path> --gate {K3,W3} [--nm <tool>]
       check_symbols.py --nm-output <file> --gate {K3,W3}
       check_symbols.py --coff-archive --binary <a.lib> [--binary <b.lib> ...]
                        --nm <llvm-nm> --gate {K3,W3} --control <name>

Runs `nm -C <binary>` (or reads saved `nm -C` text with --nm-output) and
prints one line per distinct (pattern, symbol) pair whose demangled name
contains a forbidden pattern:

  FORBIDDEN <pattern> <symbol>

Every symbol counts, defined or undefined: K3/W3 run on the K1/W1 forced-link
executables, so a defined node symbol means node code was linked in, and an
undefined one means something still asks for it.

--coff-archive (Windows): an MSVC executable has no symbol table, so the
caller passes the probe's static link inputs instead (COFF .lib archives,
--binary repeatable). This mode is explicit, never guessed: every input must be
an archive (starts with "!<arch>\n") and --nm must be LLVM's llvm-nm (its
--version must say LLVM), which demangles MSVC names. All symbols of all
archives are scanned, defined or undefined.

--control <name> (positive control, any mode): the scan is trusted only if
the parsed output holds at least one DEFINED symbol containing "dinero::" and
one DEFINED symbol containing <name>. Otherwise it prints
"UNMEASURED <reason>" and exits 3, whatever the forbidden-symbol result.

Lines starting with '#' are informational. Exit 0 clean, 1 on any forbidden
symbol, 2 on bad input (missing binary, nm failure, no symbols parsed, a
non-archive or non-LLVM nm in --coff-archive mode), 3 unmeasured (control).
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
NM_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z?\-])\s+(.+?)\s*$")
UNDEFINED_TYPES = set("Uvw")
AR_MAGIC = b"!<arch>\n"


class InputError(Exception):
    pass


def check_llvm_nm(nm):
    try:
        r = subprocess.run([nm, "--version"], capture_output=True, text=True, errors="replace")
    except OSError as e:
        raise InputError("cannot run %s: %s" % (nm, e))
    if r.returncode != 0 or "LLVM" not in r.stdout:
        raise InputError("--coff-archive needs llvm-nm; %s --version does not say LLVM" % nm)


def run_nm(nm, binary):
    try:
        r = subprocess.run([nm, "-C", binary], capture_output=True,
                           text=True, errors="replace")
    except OSError as e:
        raise InputError("cannot run %s: %s" % (nm, e))
    if r.returncode != 0:
        raise InputError("%s -C %s failed (%d): %s"
                         % (nm, binary, r.returncode, r.stderr.strip()))
    return r.stdout


def nm_text(args):
    if args.nm_output:
        try:
            return Path(args.nm_output).read_text(errors="replace")
        except OSError as e:
            raise InputError("cannot read %s: %s" % (args.nm_output, e))
    for b in args.binary:
        if not Path(b).is_file():
            raise InputError("no such binary: %s" % b)
    if len(args.binary) > 1 and not args.coff_archive:
        raise InputError("several --binary inputs need --coff-archive")
    if args.coff_archive:
        for b in args.binary:
            with open(b, "rb") as f:
                if f.read(len(AR_MAGIC)) != AR_MAGIC:
                    raise InputError("--coff-archive: %s is not an archive" % b)
        check_llvm_nm(args.nm)
    return "".join(run_nm(args.nm, b) for b in args.binary)


def parse(text):
    """(type, name) for every nm symbol line."""
    out = []
    for line in text.splitlines():
        m = NM_LINE.match(line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def symbols(text):
    return [name for _, name in parse(text)]


def control_failure(entries, name):
    defined = [n for t, n in entries if t not in UNDEFINED_TYPES]
    if not any("dinero::" in n for n in defined):
        return "no defined demangled dinero:: symbol (nm not demangling, or wrong input)"
    if not any(name in n for n in defined):
        return "control symbol %s is not defined in the scanned input" % name
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--binary", action="append")
    src.add_argument("--nm-output")
    ap.add_argument("--gate", required=True, choices=sorted(GATES))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--coff-archive", action="store_true")
    ap.add_argument("--control")
    args = ap.parse_args(argv)
    if args.coff_archive and not args.binary:
        ap.error("--coff-archive needs --binary")

    try:
        entries = parse(nm_text(args))
        syms = [n for _, n in entries]
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
    src_name = " ".join(args.binary) if args.binary else args.nm_output
    print("# %s %s: %d symbols scanned, %d forbidden"
          % (args.gate, src_name, len(syms), len(hits)))
    if args.control:
        why = control_failure(entries, args.control)
        if why:
            print("UNMEASURED %s" % why)
            return 3
        print("# control: defined %s found" % args.control)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
