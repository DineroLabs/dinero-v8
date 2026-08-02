#!/usr/bin/env python3
"""Ratchet gate: no NEW raw assert() in tests.

WHY
---
CI builds Release, and CMAKE_CXX_FLAGS_RELEASE is "-O3 -DNDEBUG". Under NDEBUG,
assert(x) expands to ((void)0) and the expression is never compiled. A test
whose only checks are assert() therefore runs in CI, prints PASSED, and verifies
nothing.

That is not hypothetical. tests/consensus/test_consensus_core_standalone.cpp
asserted a 2,627,900 DIN premine that Dinero does not have -- referencing a
constant that had already been deleted -- and still compiled clean in CI,
because the preprocessor discarded the reference before the compiler saw it.
See issue #497.

WHAT THIS GATE DOES
-------------------
Roughly 1,900 raw assertions exist across ~89 files. Converting them all at once
is not realistic, and blocking every PR until that finishes would be worse than
the problem. So this is a RATCHET, not a cliff:

  * a file may never GAIN raw assertions beyond its recorded baseline;
  * a test file not in the baseline may not introduce any at all;
  * removing them is always allowed, and lowering a baseline entry is
    encouraged (run with --update-baseline).

The baseline only shrinks. New code is held to the correct standard immediately
while the backlog is migrated in risk order (monetary/consensus -> Utreexo ->
P2P -> other).

EXEMPTION
---------
A file that does `#undef NDEBUG` before including <cassert> has forced its
assertions on deliberately, so they do gate. Those files are exempt and counted
as zero. tests/consensus/test_header_restart_safety.cpp is the existing example.
"""

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "test_assertions_baseline.json")

# `assert(` but not `static_assert(` / `foo_assert(`. Also skips the macro's own
# definition sites and NDEBUG-forcing files (handled separately).
ASSERT_RE = re.compile(r"(?<![A-Za-z0-9_])assert\s*\(")
LINE_COMMENT_RE = re.compile(r"//.*$")
UNDEF_NDEBUG_RE = re.compile(r"^\s*#\s*undef\s+NDEBUG", re.MULTILINE)

TEST_DIRS = ("tests",)
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp")


def strip_comments(text: str) -> str:
    """Remove // comments and /* */ blocks so commented-out asserts do not count."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end == -1 else end + 2
            continue
        nl = text.find("\n", i)
        if nl == -1:
            nl = n
        out.append(LINE_COMMENT_RE.sub("", text[i:nl]))
        i = nl + 1
    return "\n".join(out)


def count_asserts(path: str):
    """Return (count, exempt). Exempt files force NDEBUG off, so their asserts gate."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except OSError:
        return 0, False
    if UNDEF_NDEBUG_RE.search(raw):
        return 0, True
    return len(ASSERT_RE.findall(strip_comments(raw))), False


def scan(root: str):
    counts = {}
    for test_dir in TEST_DIRS:
        base = os.path.join(root, test_dir)
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root)
                count, exempt = count_asserts(full)
                if count > 0 and not exempt:
                    counts[rel] = count
    return counts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite the baseline from the current tree "
                         "(only legitimate when counts went DOWN)")
    args = ap.parse_args()

    current = scan(REPO_ROOT)

    if args.update_baseline:
        with open(BASELINE_PATH, "w", encoding="utf-8") as fh:
            json.dump(current, fh, indent=2, sort_keys=True)
            fh.write("\n")
        total = sum(current.values())
        print(f"baseline updated: {len(current)} files, {total} raw assertions")
        return 0

    if not os.path.exists(BASELINE_PATH):
        print(f"ERROR: missing baseline {BASELINE_PATH}", file=sys.stderr)
        print("Generate it with: python3 scripts/ci/check_test_assertions.py "
              "--update-baseline", file=sys.stderr)
        return 2

    with open(BASELINE_PATH, "r", encoding="utf-8") as fh:
        baseline = json.load(fh)

    regressions = []
    for path, count in sorted(current.items()):
        allowed = baseline.get(path)
        if allowed is None:
            regressions.append(
                f"  {path}: {count} raw assert() in a file with no baseline entry\n"
                f"      New tests must use always-on checks. assert() is compiled\n"
                f"      out under NDEBUG, so these would not gate in CI.")
        elif count > allowed:
            regressions.append(
                f"  {path}: {count} raw assert(), baseline allows {allowed} "
                f"(+{count - allowed})\n"
                f"      Existing assertions here are grandfathered, but new ones\n"
                f"      may not be added -- they would not gate in CI.")

    improved = [(p, baseline[p], current.get(p, 0))
                for p in baseline
                if current.get(p, 0) < baseline[p]]

    total_current = sum(current.values())
    total_baseline = sum(baseline.values())
    print(f"raw assert() in tests: {total_current} across {len(current)} files "
          f"(baseline {total_baseline} across {len(baseline)})")

    if improved:
        print(f"\n{len(improved)} file(s) improved since the baseline:")
        for path, was, now in sorted(improved)[:20]:
            print(f"  {path}: {was} -> {now}")
        print("Run: python3 scripts/ci/check_test_assertions.py --update-baseline")

    if regressions:
        print("\nFAILED: new raw assert() introduced\n", file=sys.stderr)
        print("\n".join(regressions), file=sys.stderr)
        print("\nUse an always-on check instead -- a plain if-statement, an\n"
              "explicit non-zero exit, or GTest EXPECT_*/ASSERT_*. See\n"
              "tests/consensus/test_subsidy_schedule.cpp for the pattern, and\n"
              "issue #497 for background.", file=sys.stderr)
        return 1

    print("\nOK: no new raw assert() introduced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
