#!/usr/bin/env python3
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Execution gate: no test may silently stop being run by any lane.

check_ctest_integrity.py proves a registered test is *runnable*. This proves it
is *run*. Those are different failures, and the second is invisible: the
workflow goes green while a gate it advertises never executes.

Three high-value tests were registered-but-never-executed on one branch. The
last was InvalidityRestartSticky, which carries the mutation proof for the
activation-lock invariant -- remove that lock and it exits 1. It was
label-excluded from the main ctest step and absent from the serial e2e list, so
nothing ran it, and a change dropping the lock would have merged green. Each
time, the only thing that caught it was a human grepping the passing log for
the test's name.

WHY NOT KEY ON THE 'mandatory' LABEL: the first version of this script did.
It did not catch the real bug -- InvalidityRestartSticky is not labelled
mandatory (its labels are integration;restart;invalidity;canonicality;...).
Whether a test is run must not depend on whether someone remembered a label.
So the rule is: if no lane runs it, it is dead coverage.

WHY THE PARSER MODELS EVERY INVOCATION: the second version keyed on "the main
ctest step OR the serial e2e list", which is 2 of the ~18 ways this repo's
workflows select tests. It reported 78 dead tests, and at least three of those
-- AssumeUtxoLateWalletImport, CsnSpendReorgReconciliation, ConsensusUTXOSetFuzz
-- have their own dedicated steps and demonstrably DO run. A guard that models
some selection mechanisms and silently ignores the rest produces confident
wrong answers, which is worse than no guard. So every ctest invocation is
parsed, and anything the parser does not understand is FATAL rather than
ignored -- see UNMODELED_FLAGS.

A test is EXECUTED if any ctest invocation in the given workflows selects it:
  * a broad invocation (no -R, no -L), minus its --label-exclude/--exclude-regex
  * an -R name/regex selection, including `for t in A B C; do ... -R "^${t}$"`
  * an -L label selection, minus that same invocation's --label-exclude
An invocation carrying -N/--show-only is an INVENTORY listing, not a run, and
is never credited. This repo has two such steps.

Remaining dead coverage is recorded in a baseline file. The gate is:

    any test that is unexecuted AND not in the baseline  ->  FAIL

New dead coverage cannot appear silently; existing debt is visible and can be
retired a category at a time. A baseline entry that starts being executed, or
that names a test which no longer exists, also fails -- the baseline shrinks
rather than rots.

Usage:
    check_mandatory_tests_execute.py <build-dir> <workflow.yml>... [--baseline P]
    ... --update      rewrite the baseline instead of checking it
    ... --explain     print the executing lane for every test and exit

Scope, so a green result is not over-read:
  * Only tests REGISTERED under this build's configuration are visible. A
    feature flag that is OFF removes its tests from registration entirely.
  * This proves a lane would SELECT the test, not that it passed.
  * Credit is given only by lanes running against THIS build directory. A lane
    using a different --test-dir (a different cmake configuration) is reported
    but not credited: whether the test is even registered there is unverified,
    and over-crediting is the exact blindness this guard exists to remove.
"""

import json
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASELINE = os.path.join(HERE, "unexecuted_tests_baseline.txt")

# Flags that do not affect WHICH tests are selected.
IGNORABLE_FLAGS = {
    "--output-on-failure", "--no-tests=error", "--parallel", "--verbose", "-V",
    "--extra-verbose", "-VV", "--quiet", "-Q", "--stop-on-failure", "--progress",
    "--schedule-random", "--force-new-ctest-process", "--repeat-until-fail",
    "--test-dir", "--timeout", "--repeat", "--output-junit", "--output-log", "-O",
    "--test-output-size-passed", "--test-output-size-failed", "--build-config", "-C",
    "--interactive-debug-mode", "--debug", "--stop-time",
}
# Flags that DO affect selection and that this parser models.
SELECTION_FLAGS = {
    "-R", "--tests-regex", "-E", "--exclude-regex", "-L", "--label-regex",
    "-LE", "--label-exclude", "-N", "--show-only",
}
# Flags that affect selection and that this parser does NOT model. Encountering
# one is FATAL. This list is the guard against repeating the exact bug that
# invalidated the previous version: a selection mechanism silently unnoticed.
UNMODELED_FLAGS = {
    "-I", "--tests-information", "--rerun-failed", "--union", "-U",
    "-FA", "--fixture-exclude-any", "-FS", "--fixture-exclude-setup",
    "-FR", "--fixture-exclude-cleanup", "--tests-from-file", "--exclude-from-file",
    "--resource-spec-file", "--test-load",
}


def registered_tests(build_dir):
    """Every registered test as (name, [labels]).

    Discovery failure is a HARD failure, never an empty result. A guard that
    treats "ctest told me nothing" as "nothing is wrong" reproduces exactly the
    blindness it exists to remove: an unconfigured or broken build directory
    would report a clean bill of health.
    """
    proc = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit("FATAL: test discovery failed in %s (ctest exit %d)\n%s"
                 % (build_dir, proc.returncode, (proc.stderr or "").strip()[:2000]))
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        sys.exit("FATAL: could not parse ctest --show-only=json-v1 output from "
                 "%s: %s" % (build_dir, exc))
    tests = []
    for t in payload.get("tests", []):
        labels = []
        for prop in t.get("properties", []):
            if prop.get("name") == "LABELS":
                v = prop.get("value")
                labels = v if isinstance(v, list) else [v]
        tests.append((t["name"], labels))
    return tests


def ctest_blocks(text, path):
    """Every ctest invocation, with shell line-continuations joined.

    Returns (line_number, argv, preceding_text) per invocation.
    """
    lines = text.split("\n")
    blocks, i = [], 0
    while i < len(lines):
        if is_command_line(lines[i]):
            start, buf = i, []
            while i < len(lines):
                s = lines[i].rstrip()
                buf.append(s[:-1] if s.endswith("\\") else s)
                if not s.endswith("\\"):
                    break
                i += 1
            joined = " ".join(buf).strip()
            joined = joined[joined.index("ctest"):]
            try:
                argv = shlex.split(joined, comments=True)
            except ValueError as exc:
                sys.exit("FATAL: %s:%d cannot tokenize ctest invocation: %s"
                         % (path, start + 1, exc))
            # Prose, not a command. "and run ctest so source drops" in a
            # comment and the step named "Run ctest" both contain the word;
            # neither runs anything. A real invocation carries flags.
            if not any(a.startswith("-") for a in argv[1:]):
                if re.match(r"^\s*(ctest\s*$|ctest\s+[^-])", joined) and \
                   not lines[start].lstrip().startswith("#"):
                    sys.exit("FATAL: %s:%d looks like a ctest command with no "
                             "flags.\nA bare `ctest` runs the ENTIRE suite, which "
                             "would change every answer this\nguard gives, so it "
                             "must not be silently skipped as prose."
                             % (path, start + 1))
                i += 1
                continue
            blocks.append((start + 1, argv, "\n".join(lines[:start])))
        i += 1
    return blocks


def is_command_line(line):
    """True if this line plausibly INVOKES ctest, rather than mentioning it.

    tests.yml contains both a step named `- name: Run ctest` and comment prose
    ("and run ctest so source drops"). Parsing either as an invocation gives it
    no --test-dir and an empty filter set, i.e. "this lane runs everything" --
    a wrong answer produced silently, which is the failure mode this whole
    guard exists to remove.
    """
    stripped = line.lstrip()
    if stripped.startswith("#"):
        return False
    # A YAML mapping key other than `run:` is metadata (name:, id:, if:, uses:).
    m = re.match(r"^\s*(?:-\s+)?([A-Za-z_][\w-]*):\s", line)
    if m and m.group(1) != "run":
        return False
    return bool(re.search(r"(^|[\s;&|(])ctest(\s|$)", line))


def loop_values(before, var):
    """Resolve a shell loop variable from the nearest preceding `for VAR in ...`."""
    hits = re.findall(r"for\s+%s\s+in\s+([^\n;]+);\s*do" % re.escape(var), before)
    return hits[-1].split() if hits else None


def parse_block(line_no, argv, before, path):
    """One invocation -> {test_dir, show_only, include, label_include,
    label_exclude, name_exclude, names}."""
    spec = {"line": line_no, "test_dir": None, "show_only": False,
            "include": None, "label_include": None, "label_exclude": None,
            "name_exclude": None, "names": None}
    i = 1
    while i < len(argv):
        tok = argv[i]
        if tok in UNMODELED_FLAGS:
            sys.exit("FATAL: %s:%d uses ctest flag %s, which this guard does not "
                     "model.\nIt changes which tests are selected, so ignoring it "
                     "would make this guard\nreport confident wrong answers -- the "
                     "exact failure it exists to prevent.\nTeach parse_block() "
                     "about %s, then regenerate the baseline."
                     % (path, line_no, tok, tok))
        if tok in ("-N", "--show-only") or tok.startswith("--show-only="):
            spec["show_only"] = True
        elif tok == "--test-dir":
            i += 1; spec["test_dir"] = argv[i]
        elif tok in ("-R", "--tests-regex"):
            i += 1; spec["include"] = argv[i]
        elif tok in ("-E", "--exclude-regex"):
            i += 1; spec["name_exclude"] = argv[i]
        elif tok in ("-L", "--label-regex"):
            i += 1; spec["label_include"] = argv[i]
        elif tok in ("-LE", "--label-exclude"):
            i += 1; spec["label_exclude"] = argv[i]
        elif tok in IGNORABLE_FLAGS:
            if tok in ("--timeout", "--repeat", "--output-junit", "--output-log",
                       "-O", "--build-config", "-C", "--test-output-size-passed",
                       "--test-output-size-failed", "--stop-time"):
                i += 1
        elif re.fullmatch(r"-j\d*", tok) or tok.startswith("--parallel"):
            pass
        elif tok.startswith("-"):
            sys.exit("FATAL: %s:%d unrecognized ctest flag %r.\nThis guard "
                     "refuses to guess whether a flag it has never seen changes\n"
                     "test selection. Classify it in IGNORABLE_FLAGS, "
                     "SELECTION_FLAGS or\nUNMODELED_FLAGS in %s."
                     % (path, line_no, tok, os.path.basename(__file__)))
        i += 1

    # Resolve shell variables in the selection patterns.
    # A bare trailing '$' is a regex end-anchor, not a shell variable; only
    # '$name' / '${name}' is an expansion this parser must resolve.
    for key in ("include", "label_include"):
        pat = spec[key]
        if pat and re.search(r"\$\{?\w", pat):
            m = re.fullmatch(r"\^\$\{?(\w+)\}?\$", pat)
            vals = loop_values(before, m.group(1)) if m else None
            if vals is None:
                sys.exit("FATAL: %s:%d selection %r contains an unresolved shell "
                         "variable\nand no preceding `for ... in ...; do` explains "
                         "it. This guard cannot tell\nwhich tests run, and must not "
                         "assume none do." % (path, line_no, pat))
            spec["names"] = vals
    return spec


def selected_by(spec, tests):
    """Names this invocation would actually run."""
    if spec["show_only"]:
        return set()                      # -N lists; it does not execute
    lx = set(spec["label_exclude"].split("|")) if spec["label_exclude"] else set()
    li = re.compile(spec["label_include"]) if spec["label_include"] else None
    nx = re.compile(spec["name_exclude"]) if spec["name_exclude"] else None
    inc = re.compile(spec["include"]) if spec["include"] and not spec["names"] else None
    fixed = set(spec["names"]) if spec["names"] else None

    out = set()
    for name, labels in tests:
        if fixed is not None:
            if name not in fixed:
                continue
        elif inc is not None and not inc.search(name):
            continue
        if li is not None and not any(li.search(l) for l in labels):
            continue
        if any(l in lx for l in labels):
            continue
        if nx is not None and nx.search(name):
            continue
        out.add(name)
    return out


def same_build_dir(a, b):
    """Does a workflow's --test-dir name the directory we discovered tests in?

    The workflow writes it relative to the checkout ("build-tests"); a caller
    may pass an absolute path. Compare on the final component, which is what
    actually distinguishes this repo's three build configurations
    (build-tests, build-tests-quic, build-core-heavy). Ambiguity is rejected
    in execution_map rather than resolved by guessing.
    """
    if a is None or b is None:
        return False
    return os.path.basename(os.path.normpath(a)) == \
           os.path.basename(os.path.normpath(b))


def execution_map(tests, workflows, build_dir):
    """name -> [lanes running it], plus the same for other build dirs."""
    same, other, broad_seen, blocks_seen = {}, {}, False, 0
    seen_dirs = {}
    for path in workflows:
        text = open(path, encoding="utf-8").read()
        for line_no, argv, before in ctest_blocks(text, path):
            blocks_seen += 1
            spec = parse_block(line_no, argv, before, path)
            if spec["show_only"]:
                continue
            lane = "%s:%d" % (os.path.basename(path), line_no)
            if spec["test_dir"]:
                key = os.path.basename(os.path.normpath(spec["test_dir"]))
                prev = seen_dirs.setdefault(key, spec["test_dir"])
                if prev != spec["test_dir"]:
                    sys.exit("FATAL: two different --test-dir values share the "
                             "final path component %r\n  %s\n  %s\nThis guard "
                             "matches build directories on that component, so it "
                             "cannot tell\nthem apart. Rename one, or teach "
                             "same_build_dir() to disambiguate."
                             % (key, prev, spec["test_dir"]))
            tgt = same if same_build_dir(spec["test_dir"], build_dir) else other
            if tgt is same and not spec["include"] and not spec["label_include"]:
                broad_seen = True
            for n in selected_by(spec, tests):
                tgt.setdefault(n, []).append(lane)
    if blocks_seen == 0:
        sys.exit("FATAL: no ctest invocation found in %s -- this guard would "
                 "check nothing" % ", ".join(workflows))
    if not broad_seen:
        sys.exit("FATAL: no broad ctest invocation (one with neither -R nor -L) "
                 "runs against\n%s. The main lane is what executes the bulk of "
                 "the suite; if it has\ndisappeared, coverage has collapsed and "
                 "this guard must say so." % build_dir)
    return same, other


def read_baseline(path):
    if not os.path.exists(path):
        return None
    names = set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names


def reason_for(name, labels, other_lanes, workflows):
    if other_lanes:
        return "selected only in another build configuration (%s); registration " \
               "there is unverified" % ", ".join(sorted(set(other_lanes)))
    excl = set()
    for path in workflows:
        for m in re.finditer(r"--label-exclude\s+'([^']+)'", open(path).read()):
            excl |= set(m.group(1).split("|"))
    hit = sorted(l for l in labels if l in excl)
    if hit:
        return "excluded by label: " + ",".join(hit)
    return "not selected by any lane"


def write_baseline(path, dead, labels_of, other, workflows):
    groups = {}
    for n in sorted(dead):
        groups.setdefault(
            reason_for(n, labels_of.get(n, []), other.get(n), workflows), []).append(n)
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "# Tests registered but executed by NO CI lane.\n"
            "# Generated by scripts/ci/check_mandatory_tests_execute.py --update\n"
            "#\n"
            "# This file is DEBT, not policy. An entry is retired by making the\n"
            "# test actually run -- add it to a serial lane, give it its own\n"
            "# step, or drop the label that excludes it -- not by leaving it\n"
            "# here. A NEW entry means NEW dead coverage and needs a reason in\n"
            "# the PR that adds it.\n"
            "#\n"
            "# Grouped by why each test is currently unexecuted.\n")
        for reason in sorted(groups):
            f.write("\n# --- %s (%d) ---\n" % (reason, len(groups[reason])))
            for n in groups[reason]:
                f.write(n + "\n")
    return groups


def main():
    argv = sys.argv[1:]
    update = "--update" in argv
    explain = "--explain" in argv
    baseline_path = DEFAULT_BASELINE
    if "--baseline" in argv:
        baseline_path = argv[argv.index("--baseline") + 1]
        del argv[argv.index("--baseline"):argv.index("--baseline") + 2]
    pos = [a for a in argv if not a.startswith("--")]
    if len(pos) < 2:
        sys.exit(__doc__)
    build_dir, workflows = pos[0], pos[1:]
    for w in workflows:
        if not os.path.exists(w):
            sys.exit("FATAL: workflow not found: %s" % w)

    tests = registered_tests(build_dir)
    if not tests:
        sys.exit("FATAL: ctest reported zero registered tests -- a filter matching "
                 "nothing is a silent-disappearance mode, not a pass")
    labels_of = dict(tests)
    same, other = execution_map(tests, workflows, build_dir)

    if explain:
        for name, _ in sorted(tests):
            lanes = same.get(name) or []
            print("%-52s %s" % (name, ",".join(lanes) if lanes else
                                ("OTHER-DIR:" + ",".join(other.get(name, []))
                                 if other.get(name) else "*** UNEXECUTED ***")))
        return 0

    dead = {n for n, _ in tests} - set(same)
    print("registered: %d   executed here: %d   unexecuted: %d"
          % (len(tests), len(same), len(dead)))

    if update:
        groups = write_baseline(baseline_path, dead, labels_of, other, workflows)
        print("baseline written: %d unexecuted test(s) -> %s" % (len(dead), baseline_path))
        for reason in sorted(groups):
            print("  %3d  %s" % (len(groups[reason]), reason))
        return 0

    baseline = read_baseline(baseline_path)
    if baseline is None:
        sys.exit("FATAL: baseline not found at %s. Generate it with --update and "
                 "review the contents before committing." % baseline_path)

    registered_names = {n for n, _ in tests}
    newly_dead = sorted(dead - baseline)
    # Two ways a baseline entry goes wrong, and neither may be silent:
    #   revived -- still registered, now executed: the debt was paid, so the
    #              entry must go or the baseline stops describing reality.
    #   stale   -- no longer registered at all (renamed or deleted): the entry
    #              now excuses a test that does not exist, and would silently
    #              excuse a DIFFERENT test if the name were ever reused.
    revived = sorted((baseline & registered_names) - dead)
    stale = sorted(baseline - registered_names)
    failed = False

    if newly_dead:
        print("\nFAIL: registered but executed by no lane, and not in the baseline:\n")
        for n in newly_dead:
            print("  %-48s labels: %s" % (n, ";".join(labels_of.get(n, [])) or "-"))
        print("\nThey look like coverage and will keep passing green while never\n"
              "running. Give each one a lane, or add it to the baseline WITH a\n"
              "reason in the PR.")
        failed = True

    if revived:
        print("\nFAIL: these baseline entries are now executed -- remove them:\n")
        for n in revived:
            print("  %-48s now run by: %s" % (n, ",".join(same.get(n, []))))
        print("\nA baseline that keeps paid-off debt stops describing reality,\n"
              "and the next reader cannot tell which entries still matter.")
        failed = True

    if stale:
        print("\nFAIL: these baseline entries name tests that are no longer registered:\n")
        for n in stale:
            print("  %s" % n)
        print("\nThey were renamed or deleted. A stale entry excuses a test that\n"
              "does not exist, and would silently excuse a DIFFERENT test if the\n"
              "name were reused. Remove them, or correct them to the new name.")
        failed = True

    if failed:
        return 1
    print("OK: no new dead coverage, no stale or paid-off baseline entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
