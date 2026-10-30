#!/usr/bin/env python3
"""Final GATE line for K4 and W5 in the separation-gate workflows.

usage: gate_label.py k4 --output FILE --rc N --reference-present {0,1}
                        --unresolved FILE [--github-output FILE]
       gate_label.py w5 --log FILE --rc N --reference-present {0,1}
                        --platform {linux,macos,windows} [--github-output FILE]

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
    symbol baseline exists; --platform the lane's platform.
    Windows W5 is PARTIAL by platform: NodeCore is not a Windows target, so
    only the wallet FFI and the RPC transcript are checked there and a pass
    reads `GATE W5 PASS (partial: wallet FFI + RPC only; NodeCore N/A on
    Windows)`, never a bare PASS. Its missing-reference verdict needs the
    wallet_ffi.h PASS line and the NodeCore N/A lines. A NodeCore item checked
    on Windows, or reported N/A on Linux/macOS (where it is required), is a
    contradiction: "check did not run".
    B1 (owner decision 3, Windows only): when SepRpcTranscript ***Failed with
    an rpc_transcript startup RUN ERROR whose error output / daemon log tail
    contains B1's exact error `invalid --consensus-state-commitment-height
    value: 4294967295` (dinerod main.cpp std::stol overflow on LLP64), the
    transcript is UNMEASURED (blocked by B1), not a FAIL. The wallet-FFI part
    still reports its own result: PASS or missing reference gives
    `GATE W5 UNMEASURED (...)` (exit 3), a wallet-FFI failure gives
    `GATE W5 FAIL ...`; both name the transcript state and the partial scope.
    Any other RUN ERROR, and the same text on Linux/macOS, stays FAIL
    "check did not run".

Prints one `GATE <id> ...` line and exits with the gate rc: 0 PASS, 1 FAIL,
2 FAIL (missing reference), 3 UNMEASURED (W5 on Windows under B1). Every
non-PASS verdict writes why=<reason> to --github-output (the summary row
shows it), plus state=UNMEASURED or note=missing reference when that is the
verdict. Stdlib only.
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
W5_ITEM_FAIL = re.compile(r"\s*(\d+: )?FAIL (nodecore_ffi\.h|wallet_ffi\.h|jni_exports\.txt|symbols_[a-z0-9]+_(off|on)\.txt)\s*$")
W5_ITEMS_WINDOWS = ("wallet_ffi.h",)
W5_NODECORE_NA = ("nodecore_ffi.h", "jni_exports.txt")
W5_PARTIAL = "partial: wallet FFI + RPC only; NodeCore N/A on Windows"
W5_NODECORE_CHECKED = re.compile(r"^\s*(\d+: )?(PASS|FAIL) (nodecore_ffi\.h|jni_exports\.txt)\s*$", re.M)
W5_NODECORE_NA_LINE = re.compile(r"^\s*(\d+: )?N/A (nodecore_ffi\.h|jni_exports\.txt|nodecore_ffi symbols) ", re.M)
B1_ERROR = re.compile(r"invalid --consensus-state-commitment-height value: 4294967295\s*$")
B1_WHY = ("transcript blocked by B1 (dinerod --consensus-state-commitment-height=4294967295 "
          "std::stol overflow on LLP64, src/daemon/main.cpp:565)")
RESULT_LINE = re.compile(r"Test +#\d+: (\S+) ")
CTEST_TAIL = re.compile(r"^\s*\d+% tests passed")
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


def sections(lines):
    """{test name: [indexes of its result line and the output ctest printed after it]}."""
    out, cur = {}, None
    for i, l in enumerate(lines):
        m = RESULT_LINE.search(l)
        if m:
            cur = m.group(1)
            out[cur] = []
        elif CTEST_TAIL.match(l):
            cur = None
        if cur is not None:
            out[cur].append(i)
    return out


def b1_blocked(section):
    """SepRpcTranscript's own output shows a startup RUN ERROR carrying B1's exact error."""
    run_error = any("rpc_transcript: RUN ERROR:" in l and "during startup" in l for l in section)
    verdict = any("rpc_transcript: FAIL transcript differs" in l for l in section)
    return run_error and not verdict and any(B1_ERROR.search(l) for l in section)


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
    if a.platform == "windows" and status["SepRpcTranscript"].startswith("***Failed"):
        idx = sections(lines).get("SepRpcTranscript", [])
        if b1_blocked([lines[i] for i in idx]):
            return w5_b1(a, lines, idx, status)
    return w5_full(a, log, lines, status)


def w5_b1(a, lines, idx, status):
    """Windows under B1: the transcript is UNMEASURED. The wallet FFI is judged
    alone by the same rules, on the log without the transcript's own output
    (its result line counted as Passed)."""
    drop = set(idx[1:])
    rest = [l for i, l in enumerate(lines) if i not in drop]
    rest = [re.sub(r"(SepRpcTranscript \.*\s*)\*\*\*Failed", r"\1  Passed", l) for l in rest]
    sub = argparse.Namespace(**vars(a))
    sub.rc = 0 if status["SepInterfaceFreeze"] == "Passed" else max(a.rc, 1)
    rc, why = w5_full(sub, "\n".join(rest) + "\n", rest,
                      {"SepInterfaceFreeze": status["SepInterfaceFreeze"], "SepRpcTranscript": "Passed"})
    if rc == 0 or why == MISSING_W5:
        a.state = "UNMEASURED"
        return 3, "%s; wallet FFI symbols: %s; %s" % (B1_WHY, "PASS" if rc == 0 else why, W5_PARTIAL)
    return rc, "wallet FFI symbols: %s; %s; %s" % (why, B1_WHY, W5_PARTIAL)


def w5_full(a, log, lines, status):
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
    windows = a.platform == "windows"
    if windows:
        m = W5_NODECORE_CHECKED.search(log)
        if m:
            return max(a.rc, 1), ("check did not run: NodeCore item checked on Windows (%s; NodeCore "
                                  "is not a Windows target)" % m.group(0).strip())
    else:
        m = W5_NODECORE_NA_LINE.search(log)
        if m:
            return max(a.rc, 1), ("check did not run: NodeCore reported N/A on %s (%s); NodeCore "
                                  "checks are required here" % (a.platform, m.group(0).strip()))
    # ctest prints a passing test's own output only with -V, so read the verdict
    # from ctest's result line.
    transcript_ok = status["SepRpcTranscript"] == "Passed"
    if not a.reference_present:
        if not transcript_ok:
            if first(lines, lambda l: "rpc_transcript: FAIL transcript differs" in l):
                return 1, "transcript mismatch (symbol reference also missing)"
            return max(a.rc, 1), "check did not run: SepRpcTranscript failed without a transcript verdict"
        if re.search(r"^\s*(\d+: )?FAIL (nodecore_ffi\.h|wallet_ffi\.h|jni_exports\.txt)", log, re.M):
            return 1, "interface mismatch (symbol reference also missing)"
        # Positive evidence that the freeze compared every other item and
        # stopped only on the absent symbol baseline.
        missing = [i for i in (W5_ITEMS_WINDOWS if windows else W5_ITEMS)
                   if not re.search(r"^\s*(\d+: )?PASS %s\s*$" % re.escape(i), log, re.M)]
        if windows:
            missing += ["N/A " + i for i in W5_NODECORE_NA
                        if not re.search(r"^\s*(\d+: )?N/A %s " % re.escape(i), log, re.M)]
        if not freeze_err:
            missing.append("no-baseline error for the symbol list")
        if not status["SepInterfaceFreeze"].startswith("***Failed"):
            missing.append("SepInterfaceFreeze ***Failed (it %s)" % status["SepInterfaceFreeze"])
        if missing:
            return max(a.rc, 1), ("check did not run: interface_freeze did not reach the "
                                  "symbol comparison (no " + "; no ".join(missing) + ")")
        return 2, MISSING_W5
    # Reference present: PASS needs rc 0, both tests Passed and no FAIL line;
    # a FAIL needs each failed test's own verdict line. Anything else is a
    # contradiction or an unmeasured failure, never PASS or a bare FAIL.
    item_fail = first(lines, lambda l: W5_ITEM_FAIL.match(l))
    transcript_fail = first(lines, lambda l: "rpc_transcript: FAIL transcript differs" in l)
    passed = [t for t in W5_TESTS if status[t] == "Passed"]
    if a.rc == 0:
        if len(passed) == len(W5_TESTS) and not item_fail and not transcript_fail:
            return 0, None
        return 1, "check did not run: log contradicts rc 0 (%s)" % ", ".join(
            "%s %s" % (t, status[t]) for t in W5_TESTS)
    if len(passed) == len(W5_TESTS):
        return max(a.rc, 1), "check did not run: log contradicts rc %d (both tests Passed)" % a.rc
    found = []
    for t, verdict, what in (("SepInterfaceFreeze", item_fail, "interface"),
                             ("SepRpcTranscript", transcript_fail, "transcript")):
        if status[t] == "Passed":
            if verdict:
                return max(a.rc, 1), "check did not run: log contradicts itself (%s Passed, %s)" % (t, verdict)
            continue
        if not verdict:
            return max(a.rc, 1), "check did not run: %s failed without a%s %s verdict" % (
                t, "n" if what[0] == "i" else "", what)
        found.append(verdict)
    return 1, "mismatch: " + "; ".join(found)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="gate", required=True)
    p4 = sub.add_parser("k4")
    p4.add_argument("--output", required=True)
    p4.add_argument("--unresolved", required=True)
    p5 = sub.add_parser("w5")
    p5.add_argument("--log", required=True)
    p5.add_argument("--platform", required=True, choices=("linux", "macos", "windows"))
    for p in (p4, p5):
        p.add_argument("--rc", type=int, required=True)
        p.add_argument("--reference-present", type=int, choices=(0, 1), required=True)
        p.add_argument("--github-output")
    a = ap.parse_args(argv)
    rc, why = (k4 if a.gate == "k4" else w5)(a)
    unmeasured = getattr(a, "state", None) == "UNMEASURED"
    missing_ref = why in (MISSING_K4, MISSING_W5)
    if rc == 2 and not missing_ref:
        rc = 1  # exit 2 means "missing reference" only
    if rc == 3 and not unmeasured:
        rc = 1  # exit 3 means UNMEASURED only
    gid = a.gate.upper()
    out = []
    if rc == 0:
        partial = a.gate == "w5" and a.platform == "windows"
        print("GATE %s PASS%s" % (gid, " (%s)" % W5_PARTIAL if partial else ""))
    elif unmeasured:
        print("GATE %s UNMEASURED (%s)" % (gid, why))
        out += ["state=UNMEASURED", "why=" + why]
    else:
        why = why or "no reason recorded"
        print("GATE %s FAIL rc=%d (%s)" % (gid, rc, why))
        out.append("why=" + why)
    if missing_ref:
        out.append("note=missing reference")
    if out and a.github_output:
        with open(a.github_output, "a") as f:
            f.write("".join(l + "\n" for l in out))
    return rc


if __name__ == "__main__":
    sys.exit(main())
