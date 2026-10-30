#!/usr/bin/env python3
"""Final GATE line for K4 and W5 in the separation-gate workflows.

usage: gate_label.py k4 --output FILE --rc N --reference-present {0,1}
                        --unresolved FILE [--github-output FILE]
       gate_label.py w5 --log FILE --rc N --reference-present {0,1}
                        [--github-output FILE]

A missing committed reference is missing evidence. It is reported as
"missing reference" ONLY when the checks actually ran and nothing else went
wrong; a build break, a test that did not run, or a runtime/input error is a
real failure and reported as "FAIL rc=<n> (check did not run: <reason>)".

k4: --output is run_consensus_vectors.py's output (the comparison against the
    platform's reference, or, without one, the informational comparison against
    the macOS reference); --rc its exit code; --unresolved the list written by
    sep_targets.py --unresolved-out (must exist: a missing file means target
    resolution never ran).
w5: --log is the ctest log of SepInterfaceFreeze + SepRpcTranscript; --rc the
    ctest exit code; --reference-present whether this platform's committed
    symbol baseline exists.

Prints one `GATE <id> ...` line (plus a `note=missing reference` line to
--github-output when that is the verdict) and exits with the gate rc: 0 PASS,
1 FAIL, 2 FAIL (missing reference). Stdlib only.
"""
import argparse
import re
import sys

K4_NOT_RUN = ("NOT_RUN ", "UNEXPECTED ", "ON_FLOOR ", "CTEST_RC ", "ERROR ", "CONFIG_MISMATCH ")
W5_TESTS = ("SepInterfaceFreeze", "SepRpcTranscript")
W5_RUN_ERRORS = ("rpc_transcript: RUN ERROR", "rpc_transcript: dinerod not executable",
                 "rpc_transcript: no baseline", "***Timeout")
W5_ITEMS = ("nodecore_ffi.h", "wallet_ffi.h", "jni_exports.txt")
K4_OK = re.compile(r"OK \d+ tests match reference ")
K4_MISMATCH = re.compile(r"DIGEST_MISMATCH \S")
# ASCII only: a Windows runner's piped stdout is cp1252.
MISSING_K4 = "missing reference -- not a vector mismatch"
MISSING_W5 = "missing reference -- not an interface mismatch"
W5_NO_SYMBOLS = re.compile(r"interface_freeze: INPUT ERROR: no baseline \S*symbols_[a-z0-9]+_(off|on)\.txt ")


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return None


def first(lines, pred):
    for l in lines:
        if pred(l):
            return l.strip()
    return None


def k4(a):
    unresolved = read(a.unresolved)
    if unresolved is None:
        return 1, "check did not run: K4 target resolution (sep_targets.py) did not run"
    names = unresolved.split()
    if names:
        return 1, "check did not run: K4 targets unresolved: " + " ".join(names)
    out = read(a.output)
    if out is None:
        return 1, "check did not run: no run_consensus_vectors.py output"
    lines = out.splitlines()
    bad = first(lines, lambda l: l.startswith(K4_NOT_RUN))
    if bad:
        return max(a.rc, 1), "check did not run: " + bad
    failed = first(lines, lambda l: l.startswith("FAILED "))
    if failed:
        return 1, "test failed: " + failed[len("FAILED "):]
    if a.rc not in (0, 1):
        return 1, "check did not run: run_consensus_vectors.py exited %d" % a.rc
    # Positive evidence the comparison ran: its per-test DIGEST_MISMATCH lines
    # or its final OK line, consistent with the exit code. A crash, an empty
    # output or anything unrecognized is "did not run", never a verdict.
    if first(lines, lambda l: "Traceback (most recent call last)" in l):
        return 1, "check did not run: run_consensus_vectors.py traceback (rc=%d)" % a.rc
    mismatch = first(lines, lambda l: K4_MISMATCH.match(l))
    ok = first(lines, lambda l: K4_OK.match(l))
    if not ((a.rc == 0 and ok and not mismatch) or (a.rc == 1 and mismatch and not ok)):
        return 1, "check did not run: unrecognized output rc=%d" % a.rc
    if not a.reference_present:
        return 2, MISSING_K4
    if mismatch:
        return 1, "vector mismatch: " + mismatch
    return 0, None


def w5_status(log, test):
    """ctest's result for test ("Passed", "***Failed", "***Not Run", ...)."""
    m = re.search(r"Test +#\d+: %s \.*\s*(\S.*?)\s+[\d.]+ sec" % test, log)
    return m.group(1) if m else None


def w5(a):
    log = read(a.log)
    if log is None:
        return 1, "check did not run: no ctest log"
    lines = log.splitlines()
    status = {}
    for t in W5_TESTS:
        status[t] = w5_status(log, t)
        if status[t] is None:
            return 1, "check did not run: %s not run" % t
    built = first(lines, lambda l: "SEPARATION TEST BUILD FAILED" in l)
    if built:
        return max(a.rc, 1), "check did not run: build failed: " + built.split("BUILD FAILED:", 1)[1].strip()
    for t in W5_TESTS:
        if status[t] != "Passed" and not status[t].startswith("***Failed"):
            return max(a.rc, 1), "check did not run: %s %s" % (t, status[t])
    err = first(lines, lambda l: any(m in l for m in W5_RUN_ERRORS))
    if err:
        return max(a.rc, 1), "check did not run: " + err
    if first(lines, lambda l: "Traceback (most recent call last)" in l):
        return max(a.rc, 1), "check did not run: Python traceback in the ctest log"
    freeze_err = [l.strip() for l in lines if "interface_freeze: INPUT ERROR" in l]
    other_freeze_err = [l for l in freeze_err if not W5_NO_SYMBOLS.search(l)]
    if other_freeze_err:
        return max(a.rc, 1), "check did not run: " + other_freeze_err[0]
    # ctest prints a passing test's own output only with -V, so read the verdict
    # from ctest's result line.
    transcript_ok = status["SepRpcTranscript"] == "Passed"
    if not a.reference_present:
        if not transcript_ok:
            return 1, "transcript mismatch (symbol reference also missing)"
        if re.search(r"^\s*(\d+: )?FAIL (nodecore_ffi\.h|wallet_ffi\.h|jni_exports\.txt)", log, re.M):
            return 1, "interface mismatch (symbol reference also missing)"
        # Positive evidence that the freeze compared every other item and
        # stopped only on the absent symbol baseline.
        missing = [i for i in W5_ITEMS
                   if not re.search(r"^\s*(\d+: )?PASS %s\s*$" % re.escape(i), log, re.M)]
        if not freeze_err:
            missing.append("no-baseline error for the symbol list")
        if not status["SepInterfaceFreeze"].startswith("***Failed"):
            missing.append("SepInterfaceFreeze ***Failed (it %s)" % status["SepInterfaceFreeze"])
        if missing:
            return max(a.rc, 1), ("check did not run: interface_freeze did not reach the "
                                  "symbol comparison (no " + "; no ".join(missing) + ")")
        return 2, MISSING_W5
    if a.rc != 0:
        return 1, None
    return 0, None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="gate", required=True)
    p4 = sub.add_parser("k4")
    p4.add_argument("--output", required=True)
    p4.add_argument("--unresolved", required=True)
    p5 = sub.add_parser("w5")
    p5.add_argument("--log", required=True)
    for p in (p4, p5):
        p.add_argument("--rc", type=int, required=True)
        p.add_argument("--reference-present", type=int, choices=(0, 1), required=True)
        p.add_argument("--github-output")
    a = ap.parse_args(argv)
    rc, why = (k4 if a.gate == "k4" else w5)(a)
    gid = a.gate.upper()
    if rc == 0:
        print("GATE %s PASS" % gid)
    else:
        print("GATE %s FAIL rc=%d%s" % (gid, rc, " (%s)" % why if why else ""))
    if rc == 2 and a.github_output:
        with open(a.github_output, "a") as f:
            f.write("note=missing reference\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
