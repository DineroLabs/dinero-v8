#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-test for the execution gate's workflow parser.

The parser is the load-bearing part of check_mandatory_tests_execute.py, and it
has already been wrong once in a way that produced a confident wrong answer: an
earlier version modelled only the main ctest step and the serial e2e loop, so it
reported tests as dead coverage that had their own dedicated steps. These cases
pin every selection mechanism the repo actually uses, and -- more importantly --
pin that an UNRECOGNIZED mechanism is fatal rather than ignored.

Runs without a build directory: registered_tests() is the only part that needs
ctest, and these cases drive the parser directly.

    python3 scripts/ci/test_check_mandatory_tests_execute.py
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "gate", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "check_mandatory_tests_execute.py"))
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)

FAILURES = []


def check(name, got, want):
    if got != want:
        FAILURES.append("%s\n     got:  %r\n     want: %r" % (name, got, want))
        print("  FAIL %s" % name)
    else:
        print("  ok   %s" % name)


def executed(workflow_text, tests, build_dir="build-tests"):
    """Run the parser over an inline workflow, return the executed name set."""
    with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as f:
        f.write(wf(workflow_text))
        path = f.name
    try:
        same, _other = gate.execution_map(tests, [path], build_dir)
        return set(same)
    finally:
        os.unlink(path)


def fatal(workflow_text, tests, build_dir="build-tests"):
    """Return the FATAL message the parser exits with, or None if it did not."""
    try:
        executed(workflow_text, tests, build_dir)
        return None
    except SystemExit as e:
        return str(e.code)


def wf(steps):
    """Wrap step fragments in a minimal but REAL workflow document.

    The parser reads workflows as YAML now, so the fixtures have to be valid
    YAML documents rather than loose fragments.
    """
    return "name: t\non: push\njobs:\n  j:\n    runs-on: ubuntu-latest\n    steps:\n" + steps


BROAD = """      - name: Run ctest
        run: |
          ctest --test-dir build-tests \\
            --output-on-failure \\
            --no-tests=error \\
            --label-exclude 'integration|fuzz'
"""

T = [("Plain", []), ("Integ", ["integration"]), ("Fuzzy", ["fuzz"]),
     ("Quic", ["quic"]), ("QuicInteg", ["quic", "integration"]),
     ("Named", ["integration"]), ("LoopA", ["integration"]),
     ("LoopB", ["integration"]), ("Heavy", ["core-heavy-smoke"]),
     # 'fuzz-torture' CONTAINS 'fuzz'. Real ctest -LE 'fuzz' excludes it;
     # exact-string modelling credited it. Keeps that distinction in the suite.
     ("FuzzTorture", ["fuzz-torture"])]


def main():
    print("parser: selection mechanisms")

    # The bug that invalidated the previous version: a dedicated exact-name step.
    check("exact-name -R step is credited",
          executed(BROAD + """
      - name: named
        run: |
          ctest --test-dir build-tests --no-tests=error -j1 -R '^Named$'
""", T),
          {"Plain", "Quic", "Heavy", "Named"})

    check("for-loop list is expanded and credited",
          executed(BROAD + """
      - name: serial
        run: |
          for t in LoopA LoopB; do
            ctest --test-dir build-tests --no-tests=error -j1 -R "^${t}$"
          done
""", T),
          {"Plain", "Quic", "Heavy", "LoopA", "LoopB"})

    # -N lists tests; it does not run them. Two such steps exist in tests.yml,
    # and crediting them would mark unrun tests as covered.
    check("-N inventory step is NOT credited",
          executed(BROAD + """
      - name: inventory
        run: ctest --test-dir build-tests -N -L 'quic|integration'
""", T),
          {"Plain", "Quic", "Heavy"})

    # The QUIC lane carries -L AND --label-exclude; a quic+integration test is
    # selected by the first and dropped by the second.
    check("-L positive selection still honours that step's --label-exclude",
          executed(BROAD + """
      - name: quic
        run: |
          ctest --test-dir build-tests --no-tests=error \\
            -L 'quic' \\
            --label-exclude 'integration'
""", T),
          {"Plain", "Quic", "Heavy"})

    check("broad step honours --exclude-regex",
          executed("""
      - name: Run ctest
        run: |
          ctest --test-dir build-tests --no-tests=error \\
            --exclude-regex '^(Plain|Quic)$' \\
            --label-exclude 'integration|fuzz'
""", T),
          {"Heavy"})

    # Credit must not cross build directories: a different --test-dir is a
    # different cmake configuration, where registration is unverified.
    check("a lane on another --test-dir is not credited",
          executed(BROAD + """
      - name: core-heavy
        run: |
          ctest --test-dir build-core-heavy --no-tests=error -L core-heavy-smoke
""", T),
          {"Plain", "Quic", "Heavy"})

    # CI passes a relative --test-dir; a human debugging passes an absolute
    # one. A literal string compare made every lane look like a different
    # build configuration, so the guard reported that the main lane had
    # vanished -- a confident wrong answer from a correct-looking FATAL.
    check("an absolute build dir matches the workflow's relative --test-dir",
          executed(BROAD, T, build_dir="/somewhere/deep/build-tests"),
          {"Plain", "Quic", "Heavy"})

    check("a same-named dir under a different parent is still matched",
          executed(BROAD, T, build_dir="build-tests/"),
          {"Plain", "Quic", "Heavy"})

    # Matching on the final component only works while those components are
    # unique. If they ever collide, say so instead of guessing.
    msg = fatal(BROAD + """
      - name: collide
        run: ctest --test-dir other/build-tests --no-tests=error -R '^Named$'
""", T)
    check("colliding --test-dir final components are FATAL",
          bool(msg and "share the final path component" in msg), True)

    print("parser: real ctest/YAML/shell semantics, not an approximation of them")

    # -LE takes a REGEX matched against each label, not a set of literal names.
    # 'fuzz-torture' contains 'fuzz', so real ctest excludes it; modelling the
    # flag as exact-string membership credited it as executed. A silent
    # over-credit committed by the gate itself.
    check("--label-exclude is a regex, not a set of literal label names",
          "FuzzTorture" not in executed(BROAD, T),
          True)

    # YAML folds a `>` scalar's lines into one command. A line-based scan
    # truncated the invocation at its first line, so a FILTERED ctest read as
    # an unfiltered one and was credited with the whole suite. This repo really
    # uses folded scalars (shielded-readiness.yml).
    folded = executed("""      - name: Run ctest
        run: >-
          ctest --test-dir build-tests --no-tests=error
          --label-exclude 'integration|fuzz'
""", T)
    check("a folded scalar (run: >-) keeps its filters",
          "Integ" not in folded and "Plain" in folded, True)

    # A second ctest after && used to merge into the first spec with last-wins
    # flags, silently changing what the lane was credited with.
    msg = fatal(BROAD + """
      - name: compound
        run: cd build-tests && ctest --no-tests=error -R '^Named$'
""", T)
    check("a compound command whose ctest has no --test-dir is FATAL",
          bool(msg and "no --test-dir" in msg), True)

    # ${VAR} in -E/-LE used to pass through as a literal regex: the exclusion
    # matched nothing and vanished without a word.
    msg = fatal(BROAD + """
      - name: var-exclude
        run: |
          for x in Named; do
            ctest --test-dir build-tests --no-tests=error -E "^${x}$"
          done
""", T)
    check("a shell variable in an EXCLUSION is FATAL, not a silent no-op",
          bool(msg and "driven by a shell loop" in msg), True)

    # A -L loop enumerates LABELS. Treating its values as test NAMES made the
    # lane credit zero tests while reporting nothing wrong.
    lab = executed(BROAD + """
      - name: label-loop
        run: |
          for lbl in quic; do
            ctest --test-dir build-tests --no-tests=error -L "^${lbl}$"
          done
""", T)
    check("a -L loop variable resolves to LABELS, not test names",
          "Quic" in lab and "QuicInteg" in lab, True)

    # loop_values used to search the whole file, so a variable could resolve
    # against an unrelated job's loop -- and tests.yml has two loops named `t`.
    msg = fatal(BROAD + """
      - name: first
        run: |
          for t in Named; do
            ctest --test-dir build-tests --no-tests=error -R "^${t}$"
          done
      - name: second
        run: |
          ctest --test-dir build-tests --no-tests=error -R "^${t}$"
""", T)
    check("a loop variable does not leak across run: blocks",
          bool(msg and "unresolved shell variable" in msg), True)

    # Equals form is legal for every long flag; only --show-only= was handled,
    # so a legal respelling hard-FATALed all of CI.
    eq = executed("""      - name: Run ctest
        run: |
          ctest --test-dir=build-tests --no-tests=error --label-exclude='integration|fuzz'
""", T)
    check("--test-dir=X and --label-exclude=X (equals form) parse",
          eq == executed(BROAD, T), True)

    print("parser: prose that mentions ctest is not an invocation")

    # tests.yml really does contain a step named "Run ctest" and a header
    # comment reading "and run ctest so source drops". Parsed as invocations
    # they carry no --test-dir and no filters, i.e. "this lane runs
    # everything" -- a wrong answer produced silently.
    check("a step NAMED 'Run ctest' is not an invocation",
          executed(BROAD + """
      - name: Run ctest again
        run: echo not-a-ctest-call
""", T),
          {"Plain", "Quic", "Heavy"})

    check("a comment mentioning ctest is not an invocation",
          executed(BROAD + """
      # and run ctest so source drops are caught
      - name: unrelated
        run: echo hi
""", T),
          {"Plain", "Quic", "Heavy"})

    # A bare `ctest` selects the ENTIRE suite. Skipping it as prose would make
    # every subsequent answer wrong, so it is fatal rather than ignored.
    # A bare `ctest` selects the ENTIRE suite from the working directory.
    # It is refused for the more specific reason that its build directory is
    # unknowable without tracking `cd`, which this parser does not do.
    msg = fatal(BROAD + """
      - name: bare
        run: ctest
""", T)
    check("a bare `ctest` is FATAL (no --test-dir)",
          bool(msg and "no --test-dir" in msg), True)

    print("parser: unrecognized input is fatal, never ignored")

    # This is the case that would have caught the real bug.
    msg = fatal(BROAD + """
      - name: from-file
        run: ctest --test-dir build-tests --tests-from-file /tmp/list.txt
""", T)
    check("modelled-as-unsupported flag is FATAL",
          bool(msg and "--tests-from-file" in msg and "does not model" in msg), True)

    msg = fatal(BROAD + """
      - name: novel
        run: ctest --test-dir build-tests --some-future-flag x
""", T)
    check("never-seen flag is FATAL",
          bool(msg and "unrecognized ctest flag" in msg), True)

    msg = fatal(BROAD + """
      - name: unresolved
        run: ctest --test-dir build-tests -R "^${undefined_var}$"
""", T)
    check("unresolved shell variable in a selection is FATAL",
          bool(msg and "unresolved shell variable" in msg), True)

    msg = fatal("      - name: nothing\n        run: echo hi\n", T)
    check("no ctest invocation at all is FATAL",
          bool(msg and "no ctest invocation found" in msg), True)

    # If the main lane disappears, coverage collapses; that must not read as a
    # pass just because the remaining named steps still parse.
    msg = fatal("""
      - name: only-named
        run: ctest --test-dir build-tests --no-tests=error -R '^Named$'
""", T)
    check("no broad invocation is FATAL",
          bool(msg and "no broad ctest invocation" in msg), True)

    print("gate: baseline hygiene")

    def run_gate(baseline_body, dead_extra=""):
        """Drive main() end-to-end with a stubbed ctest discovery."""
        with tempfile.TemporaryDirectory() as d:
            wfp = os.path.join(d, "w.yml"); open(wfp, "w").write(wf(BROAD + dead_extra))
            bl = os.path.join(d, "b.txt"); open(bl, "w").write(baseline_body)
            script = gate.__file__
            env = dict(os.environ, GATE_FAKE_TESTS="1")
            # registered_tests is stubbed by monkeypatching in-process instead.
            orig = gate.registered_tests
            gate.registered_tests = lambda _b: T
            try:
                argv = sys.argv
                sys.argv = ["g", "build-tests", wfp, "--baseline", bl]
                import io, contextlib
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    rc = gate.main()
                return rc, buf.getvalue()
            finally:
                gate.registered_tests = orig
                sys.argv = argv

    # Everything unexecuted is in the baseline -> pass.
    full = "\n".join(["Integ", "Fuzzy", "QuicInteg", "Named", "LoopA", "LoopB",
                       "FuzzTorture"])
    rc, out = run_gate(full)
    check("complete baseline passes", rc, 0)

    # A test drops out of every lane and is not in the baseline -> fail.
    rc, out = run_gate("\n".join(["Integ", "Fuzzy", "QuicInteg", "Named", "LoopA",
                                  "FuzzTorture"]))
    check("new dead coverage fails", rc, 1)
    check("  and names the test", "LoopB" in out, True)

    # A baseline entry that is now executed -> fail (debt must not rot).
    rc, out = run_gate(full, """
      - name: named
        run: ctest --test-dir build-tests --no-tests=error -R '^Named$'
""")
    check("paid-off baseline entry fails", rc, 1)
    check("  and is reported as revived", "now executed" in out and "Named" in out, True)

    # A baseline entry naming a test that no longer exists -> fail.
    rc, out = run_gate(full + "\nRenamedAway")
    check("stale baseline entry fails", rc, 1)
    check("  and is reported as no longer registered",
          "no longer registered" in out and "RenamedAway" in out, True)

    print()
    if FAILURES:
        print("FAILED (%d):" % len(FAILURES))
        for f in FAILURES:
            print("  " + f)
        return 1
    print("all parser and gate cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
