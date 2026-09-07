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

import argparse
import json
import os
import re
import shlex
import subprocess
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASELINE = os.path.join(HERE, "unexecuted_tests_baseline.txt")

# Flags that do not affect WHICH tests are selected. Base names only: the
# parser splits an equals form (--no-tests=error) before looking here, so
# entries must not carry values.
IGNORABLE_FLAGS = {
    "--output-on-failure", "--no-tests", "--verbose", "-V", "--extra-verbose",
    "-VV", "--quiet", "-Q", "--stop-on-failure", "--progress",
    "--schedule-random", "--force-new-ctest-process", "--repeat-until-fail",
    "--interactive-debug-mode", "--debug", "--test-output-truncation",
}
# Selection-affecting flags are not listed here: parse_block()'s VALUED table
# IS that list, and is the one the parser consults. A second copy would be a
# table nobody reads that looks authoritative.
# Flags that affect selection and that this parser does NOT model. Encountering
# one is FATAL. This list is the guard against repeating the bug that
# invalidated an earlier version: a selection mechanism silently unnoticed.
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


def run_blocks(path):
    """Every `run:` script in the workflow, as (approx_line, script_text).

    Parsed as YAML rather than scanned line-by-line. The line-based version
    joined only backslash continuations, so a FOLDED scalar (`run: >` or
    `run: >-`, where YAML itself joins the lines) was truncated at its first
    line: a filtered ctest invocation read as an unfiltered one and was
    credited with running the whole suite. This repo does use folded scalars --
    .github/workflows/shielded-readiness.yml has two -- so that is a live
    mis-parse, not a hypothetical one.
    """
    raw = open(path, encoding="utf-8").read()
    try:
        doc = yaml.safe_load(raw)
    except yaml.YAMLError as exc:
        sys.exit("FATAL: %s is not parseable YAML: %s" % (path, exc))
    if not isinstance(doc, dict):
        sys.exit("FATAL: %s did not parse to a mapping" % path)

    lines = raw.split("\n")

    def approx_line(script):
        """Best-effort line number for reporting. YAML discards positions, so
        locate the script's first non-empty line in the raw text."""
        first = next((l.strip() for l in script.split("\n") if l.strip()), "")
        if first:
            for i, l in enumerate(lines):
                if first in l:
                    return i + 1
        return 0

    out = []
    for job in (doc.get("jobs") or {}).values():
        if not isinstance(job, dict):
            continue
        for step in (job.get("steps") or []):
            if isinstance(step, dict) and isinstance(step.get("run"), str):
                out.append((approx_line(step["run"]), step["run"]))
    return out


def split_unquoted(text):
    """Split on shell command separators, ignoring any inside quotes.

    Quote awareness is not a nicety here: the main lane carries
    --label-exclude 'integration|gate|release|canonicality|fuzz', and a naive
    split on '|' tears that argument in half.

    Splitting at all is what stops `a && ctest ... && ctest ...` from
    collapsing into one spec whose flags are silently last-wins.
    """
    parts, buf, quote, i = [], [], None, 0
    while i < len(text):
        ch = text[i]
        if quote:
            buf.append(ch)
            if ch == quote:
                quote = None
            elif ch == "\\" and quote == '"' and i + 1 < len(text):
                i += 1
                buf.append(text[i])
        elif ch in "'\"":
            quote = ch
            buf.append(ch)
        elif ch in ";\n":
            parts.append("".join(buf)); buf = []
        elif ch in "|&":
            # || && | & all separate commands
            if i + 1 < len(text) and text[i + 1] == ch:
                i += 1
            parts.append("".join(buf)); buf = []
        else:
            buf.append(ch)
        i += 1
    parts.append("".join(buf))
    return parts


def shell_commands(script):
    """Split a run: script into individual commands, continuations joined.

    Comments are dropped first: an unscoped search for `ctest` previously
    matched prose inside comments, and a `for t in ...` written in a comment
    could resolve a loop variable.
    """
    # Join backslash continuations before anything else.
    joined = re.sub(r"\\\n\s*", " ", script)
    stripped = []
    for line in joined.split("\n"):
        # A '#' starts a comment when it begins a word. Good enough here, and
        # erring toward dropping text only ever removes candidate commands.
        stripped.append(re.sub(r"(^|\s)#.*$", "", line))
    return [c.strip() for c in split_unquoted("\n".join(stripped)) if c.strip()]


def ctest_blocks(text_or_path, path=None):
    """Every ctest invocation: (approx_line, argv, enclosing_script)."""
    blocks = []
    for line_no, script in run_blocks(text_or_path if path is None else path):
        for cmd in shell_commands(script):
            # Strip leading VAR=value assignments and `env`.
            probe = re.sub(r"^(?:env\s+|[A-Za-z_][\w]*=\S*\s+)*", "", cmd)
            if not re.match(r"ctest(\s|$)", probe):
                continue
            try:
                argv = shlex.split(probe, comments=True)
            except ValueError as exc:
                sys.exit("FATAL: %s:%d cannot tokenize ctest invocation: %s"
                         % (path or text_or_path, line_no, exc))
            if not argv:
                continue
            blocks.append((line_no, argv, script))
    return blocks


def loop_values(script, var):
    """Resolve a shell loop variable from `for VAR in ...; do` in THIS script.

    Scoped to the enclosing run: block and applied to comment-stripped text.
    The previous version searched the whole file, so it could resolve a
    variable from an unrelated job's loop -- and tests.yml has two loops both
    named `t`, which is exactly the collision that makes a wrong answer look
    plausible.
    """
    clean = "\n".join(re.sub(r"(^|\s)#.*$", "", l) for l in script.split("\n"))
    hits = re.findall(r"for\s+%s\s+in\s+([^\n;]+);\s*do" % re.escape(var), clean)
    if not hits:
        return None
    # A run: block with two different loops over the same name cannot be
    # resolved to one answer; refuse rather than pick.
    values = [h.split() for h in hits]
    if len({tuple(v) for v in values}) > 1:
        return "AMBIGUOUS"
    return values[-1]


def parse_block(line_no, argv, script, path):
    """One invocation -> a spec describing exactly which tests it selects."""
    spec = {"line": line_no, "test_dir": None, "show_only": False,
            "include": None, "label_include": None, "label_exclude": None,
            "name_exclude": None, "names": None, "labels": None}

    # Flags taking a value, canonical name -> spec key.
    VALUED = {
        "--test-dir": "test_dir",
        "-R": "include", "--tests-regex": "include",
        "-E": "name_exclude", "--exclude-regex": "name_exclude",
        "-L": "label_include", "--label-regex": "label_include",
        "-LE": "label_exclude", "--label-exclude": "label_exclude",
    }
    # Value-taking flags that do not affect SELECTION.
    VALUED_IGNORED = {"--timeout", "--repeat", "--output-junit", "--output-log",
                      "-O", "--build-config", "-C", "--test-output-size-passed",
                      "--test-output-size-failed", "--stop-time", "-j",
                      "--parallel"}

    i = 1
    while i < len(argv):
        tok = argv[i]
        # Equals form is legal for every long flag. Only --show-only= was
        # handled before, so a lane written --test-dir=build-tests hard-FATALed
        # all of CI on a legal respelling.
        name, eq, value = tok.partition("=")
        has_eq = bool(eq)

        if name in UNMODELED_FLAGS:
            sys.exit("FATAL: %s:%d uses ctest flag %s, which this guard does not "
                     "model.\nIt changes which tests are selected, so ignoring it "
                     "would make this guard\nreport confident wrong answers -- the "
                     "exact failure it exists to prevent.\nTeach parse_block() "
                     "about %s, then regenerate the baseline."
                     % (path, line_no, name, name))

        if name in ("-N", "--show-only"):
            spec["show_only"] = True
        elif name in VALUED:
            if has_eq:
                spec[VALUED[name]] = value
            else:
                i += 1
                if i >= len(argv):
                    sys.exit("FATAL: %s:%d flag %s has no value" % (path, line_no, name))
                spec[VALUED[name]] = argv[i]
        elif name in VALUED_IGNORED:
            if not has_eq:
                i += 1
        elif name in IGNORABLE_FLAGS:
            pass
        elif re.fullmatch(r"-j\d*", tok):
            pass
        elif tok.startswith("-"):
            sys.exit("FATAL: %s:%d unrecognized ctest flag %r.\nThis guard "
                     "refuses to guess whether a flag it has never seen changes\n"
                     "test selection. Classify it in IGNORABLE_FLAGS, "
                     "parse_block()'s VALUED table,\nor UNMODELED_FLAGS, in %s."
                     % (path, line_no, tok, os.path.basename(__file__)))
        else:
            # A bare non-flag word. ctest takes no positional arguments, so this
            # is a command this parser did not split correctly -- exactly the
            # "everything unknown is FATAL" case the charter demands.
            sys.exit("FATAL: %s:%d unexpected argument %r in a ctest command.\n"
                     "ctest takes no positional arguments, so this parser has "
                     "mis-split a\ncompound shell command and would report a "
                     "selection that never runs."
                     % (path, line_no, tok))
        i += 1

    # Every ctest here must say which build it runs against. Without it the
    # directory comes from the shell's cwd (a preceding `cd`), which this
    # parser does not track -- so refuse rather than guess.
    if not spec["show_only"] and spec["test_dir"] is None:
        sys.exit("FATAL: %s:%d ctest invocation has no --test-dir.\nIts build "
                 "directory would come from the working directory, which this\n"
                 "guard does not model. Add --test-dir so the lane is "
                 "attributable." % (path, line_no))

    # Resolve shell variables in ALL FOUR selection patterns. Previously only
    # -R and -L were resolved, so a ${VAR} in -E or -LE passed through as a
    # literal regex, matched nothing, and the exclusion silently vanished.
    for key in ("include", "label_include", "name_exclude", "label_exclude"):
        pat = spec[key]
        if not pat or not re.search(r"\$\{?\w", pat):
            continue
        m = re.fullmatch(r"\^?\$\{?(\w+)\}?\$?", pat)
        vals = loop_values(script, m.group(1)) if m else None
        if vals == "AMBIGUOUS":
            sys.exit("FATAL: %s:%d selection %r resolves to more than one "
                     "`for` loop\nin the same run: block. This guard will not "
                     "pick one." % (path, line_no, pat))
        if vals is None:
            sys.exit("FATAL: %s:%d selection %r contains an unresolved shell "
                     "variable\nand no `for ... in ...; do` in the same run: "
                     "block explains it. This\nguard cannot tell which tests "
                     "run, and must not assume none do."
                     % (path, line_no, pat))
        # WHAT the loop values ARE depends on which flag carried them.
        # -R/-E enumerate test NAMES; -L/-LE enumerate LABELS. Treating a
        # label loop's values as names made such a lane credit zero tests
        # while reporting no problem at all.
        if key == "include":
            spec["names"] = vals
        elif key == "label_include":
            spec["labels"] = vals
        else:
            # An exclusion driven by a loop variable is a shape this parser
            # does not model; refuse rather than approximate it.
            sys.exit("FATAL: %s:%d exclusion %r is driven by a shell loop.\n"
                     "This guard models exclusions as fixed patterns and would "
                     "get the\nper-iteration semantics wrong." % (path, line_no, pat))
    return spec


def selected_by(spec, tests):
    """Names this invocation would actually run."""
    if spec["show_only"]:
        return set()                      # -N lists; it does not execute

    # -LE takes a REGEX, matched against each label, not a set of literal
    # names. Modelling it as exact-string membership silently OVER-credited:
    # with -LE 'integration|gate|release|canonicality|fuzz', a test labelled
    # 'fuzz-torture' is excluded by real ctest (the regex matches within the
    # label) but was credited as executed by the model -- the precise failure
    # this whole gate exists to detect, committed by the gate itself.
    lx = re.compile(spec["label_exclude"]) if spec["label_exclude"] else None
    li = re.compile(spec["label_include"]) if spec["label_include"] else None
    nx = re.compile(spec["name_exclude"]) if spec["name_exclude"] else None
    inc = re.compile(spec["include"]) if spec["include"] and not spec["names"] else None
    fixed = set(spec["names"]) if spec["names"] else None
    fixed_labels = set(spec["labels"]) if spec["labels"] else None

    out = set()
    for name, labels in tests:
        if fixed is not None:
            if name not in fixed:
                continue
        elif inc is not None and not inc.search(name):
            continue
        if fixed_labels is not None:
            if not any(l in fixed_labels for l in labels):
                continue
        elif li is not None and not any(li.search(l) for l in labels):
            continue
        if lx is not None and any(lx.search(l) for l in labels):
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
    (build-tests, build-tests-quic, build-core-heavy). Ambiguity is rejected in
    execution_map rather than resolved by guessing.
    """
    if a is None or b is None:
        return False
    return os.path.basename(os.path.normpath(a)) == \
           os.path.basename(os.path.normpath(b))


def execution_map(tests, workflows, build_dir, require_broad=True,
                  collect_excludes=None):
    """name -> [lanes running it], plus the same for other build dirs."""
    same, other, broad_seen, blocks_seen = {}, {}, False, 0
    seen_dirs = {}
    for path in workflows:
        for line_no, argv, script in ctest_blocks(path, path):
            blocks_seen += 1
            spec = parse_block(line_no, argv, script, path)
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
            if collect_excludes is not None and spec["label_exclude"] \
                    and same_build_dir(spec["test_dir"], build_dir):
                collect_excludes.append(re.compile(spec["label_exclude"]))
            tgt = same if same_build_dir(spec["test_dir"], build_dir) else other
            if tgt is same and not spec["include"] and not spec["label_include"] \
                    and not spec["names"] and not spec["labels"]:
                broad_seen = True
            for n in selected_by(spec, tests):
                tgt.setdefault(n, []).append(lane)
    if blocks_seen == 0:
        sys.exit("FATAL: no ctest invocation found in %s -- this guard would "
                 "check nothing" % ", ".join(workflows))
    # A scoped secondary configuration legitimately has no broad lane: the
    # QUIC job runs only -L 'quic|p2p'. Requiring one there would FATAL on a
    # correct workflow. For the PRIMARY config the requirement stands -- if the
    # main lane disappears, coverage collapses and a green run would be a lie.
    if require_broad and not broad_seen:
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


def reason_for(name, labels, other_lanes, exclude_patterns):
    """Why this test is unexecuted, for the baseline's grouping.

    Takes the label-exclude patterns the PARSER already extracted. The previous
    version re-scanned the raw workflow text with a single-quote-only regex and
    then did exact-string membership on the split parts -- repeating, in the
    explanation, the very bug that was just fixed in the selection logic. A
    'fuzz-torture' label would have been reported as "not selected by any lane"
    when the real reason is that -LE 'fuzz' matches it.
    """
    if other_lanes:
        # File name only, no line number. This string is written into a
        # CHECKED-IN file, and a line number shifts whenever anything above it
        # in the workflow moves -- producing diff noise that says nothing about
        # coverage. Same reason the baseline keys on test names rather than
        # CTest numbers. --explain still reports file:line for locating a lane.
        files = sorted({lane.split(":", 1)[0] for lane in other_lanes})
        return "selected only in another build configuration (%s); registration " \
               "there is unverified" % ", ".join(files)
    hit = sorted(l for l in labels
                 if any(pat.search(l) for pat in exclude_patterns))
    if hit:
        return "excluded by label: " + ",".join(hit)
    return "not selected by any lane"


def write_baseline(path, dead, labels_of, other, exclude_patterns):
    groups = {}
    for n in sorted(dead):
        groups.setdefault(
            reason_for(n, labels_of.get(n, []), other.get(n),
                       exclude_patterns), []).append(n)
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
    # argparse, like the sibling CI scripts. The hand-rolled parser had two
    # real defects: a trailing `--baseline` raised IndexError instead of a
    # message, and `--baseline=PATH` was silently DROPPED -- so `--update`
    # rewrote the committed default baseline rather than the requested file,
    # destroying checked-in state while reporting success.
    ap = argparse.ArgumentParser(
        description="Fail when a registered test is executed by no CI lane.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("build_dir", help="configured build directory to discover tests in")
    ap.add_argument("workflows", nargs="+", help="workflow YAML files that run ctest")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE,
                    help="known-unexecuted list (default: %(default)s)")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the baseline instead of checking it")
    ap.add_argument("--explain", action="store_true",
                    help="print the executing lane for every test and exit")
    ap.add_argument("--only-absent-from", metavar="BUILD_DIR",
                    help="restrict the check to tests NOT registered in "
                         "BUILD_DIR. Use when gating a second cmake "
                         "configuration: tests it shares with the primary "
                         "config are that config's gate's responsibility, and "
                         "listing them here would bury this config's unique "
                         "tests under hundreds of irrelevant entries.")
    args = ap.parse_args()

    build_dir, workflows = args.build_dir, args.workflows
    baseline_path, update, explain = args.baseline, args.update, args.explain
    for w in workflows:
        if not os.path.exists(w):
            sys.exit("FATAL: workflow not found: %s" % w)

    tests = registered_tests(build_dir)
    if args.only_absent_from:
        # A test registered in BOTH configs is gated by the primary
        # invocation. What no other gate can see is this config's UNIQUE
        # registry -- e.g. the QUIC family, which exists only when
        # DINERO_ENABLE_QUIC=ON. RelayTlsKeypair is in that set, and it is the
        # test the CTest INTEGRITY gate was written for after it rotted to
        # non-compiling: the same blind spot, one config over.
        shared = {n for n, _ in registered_tests(args.only_absent_from)}
        before = len(tests)
        tests = [(n, l) for n, l in tests if n not in shared]
        print("scoped to %d test(s) unique to %s (%d shared with %s are gated there)"
              % (len(tests), build_dir, before - len(tests), args.only_absent_from))
        if not tests:
            sys.exit("FATAL: no tests are unique to %s. Either the two "
                     "configurations\nnow register the same set -- in which case "
                     "this invocation is redundant\nand should be removed -- or "
                     "the comparison build dir is wrong."
                     % build_dir)
    if not tests:
        sys.exit("FATAL: ctest reported zero registered tests -- a filter matching "
                 "nothing is a silent-disappearance mode, not a pass")
    labels_of = dict(tests)
    exclude_patterns = []
    same, other = execution_map(tests, workflows, build_dir,
                                require_broad=not args.only_absent_from,
                                collect_excludes=exclude_patterns)

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
        groups = write_baseline(baseline_path, dead, labels_of, other,
                                exclude_patterns)
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
