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
A file that does `#undef NDEBUG` has forced its assertions on deliberately, so
they do gate. Such a file is exempt and counted as zero -- but ONLY when an
`#include <cassert>` (or <assert.h>) FOLLOWS the #undef.

That specific ordering is what matters, and it is not the same as "the #undef
comes first in the file". <cassert> is explicitly re-includable: the standard
requires assert to be redefined according to the current state of NDEBUG each
time the header is included. So a file may include <cassert> early (or pull it
in transitively through some other header) while NDEBUG is defined, then
#undef NDEBUG and re-include <cassert>, and assert becomes active from there.
Verified empirically, not just read off the standard: a probe including
<cassert> first under -DNDEBUG, then undefining and re-including, aborts on a
false assertion.

Conversely, a bare "#undef NDEBUG appears somewhere" match is too weak -- an
#undef with no subsequent <cassert> include changes nothing, and exempting it
would hand a free pass to a file whose assertions are still compiled out.

tests/consensus/test_header_restart_safety.cpp and test_header_sidebranch_bound.cpp
are the existing correct examples: both include other headers first, then
#undef NDEBUG, then #include <cassert>.

SCOPE -- KNOWN LIMITATION
-------------------------
SCAN_DIRS below lists the directories searched. It is not the whole repository:
compiled test code living outside those roots is NOT covered, and a raw assert()
introduced there will not be caught. The set is deliberately explicit rather
than repo-wide so that the blind spot is visible and reviewable instead of
implied. When a new compiled-test location is added to the build, add it here
too. Report actual coverage as "the directories in SCAN_DIRS", never as
"the repository".
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
CASSERT_INCLUDE_RE = re.compile(
    r"^\s*#\s*include\s*[<\"](?:cassert|assert\.h)[>\"]", re.MULTILINE)

# Directories searched. NOT the whole repo -- see "SCOPE" in the module
# docstring. Add new compiled-test roots here when the build gains them.
SCAN_DIRS = ("tests", "fuzz")
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
    """Return (count, exempt).

    Exempt means the file does `#undef NDEBUG` and then includes <cassert>
    AFTER it. That re-inclusion is what actually turns assert back on -- see the
    EXEMPTION section of the module docstring. An #undef with no subsequent
    <cassert> include does nothing, so it earns no exemption.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except OSError:
        return 0, False

    body = strip_comments(raw)

    undef = UNDEF_NDEBUG_RE.search(body)
    if undef:
        # Any <cassert> include positioned after the #undef re-arms assert.
        for inc in CASSERT_INCLUDE_RE.finditer(body):
            if inc.start() > undef.start():
                return 0, True
        # Falls through: the #undef is not followed by a <cassert> include, so
        # assert here is still whatever the earlier inclusion made it.

    return len(ASSERT_RE.findall(body)), False


def scan(root: str):
    counts = {}
    for scan_dir in SCAN_DIRS:
        base = os.path.join(root, scan_dir)
        if not os.path.isdir(base):
            continue
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


def find_regressions(current: dict, baseline: dict):
    """Files that are new to the baseline, or whose count grew."""
    out = []
    for path, count in sorted(current.items()):
        allowed = baseline.get(path)
        if allowed is None:
            out.append((path, count, None))
        elif count > allowed:
            out.append((path, count, allowed))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite the baseline from the current tree; refuses "
                         "if any count grew (the baseline may only shrink)")
    ap.add_argument("--adopt-scope", metavar="DIR", action="append", default=[],
                    help="when SCAN_DIRS gains a directory, permit new baseline "
                         "entries under DIR only. Everything outside DIR is "
                         "still held to the shrink-only rule, so this cannot be "
                         "used to launder a regression. Repeatable.")
    args = ap.parse_args()

    current = scan(REPO_ROOT)
    existing = {}
    if os.path.exists(BASELINE_PATH):
        with open(BASELINE_PATH, "r", encoding="utf-8") as fh:
            existing = json.load(fh)

    if args.update_baseline:
        # The baseline may only SHRINK. Without this check, --update-baseline
        # would launder any regression into the new "allowed" figure, which
        # would quietly turn the ratchet into a no-op.
        if existing:
            grew = find_regressions(current, existing)
            if args.adopt_scope:
                # Only brand-new entries under an explicitly adopted directory
                # are forgiven. A COUNT INCREASE in an already-tracked file is
                # still a regression even inside that directory.
                adopted = tuple(d.rstrip("/") + "/" for d in args.adopt_scope)
                newly_covered = [g for g in grew
                                 if g[2] is None and g[0].startswith(adopted)]
                grew = [g for g in grew if g not in newly_covered]
                if newly_covered:
                    total_new = sum(c for _p, c, _a in newly_covered)
                    print(f"adopting {len(newly_covered)} newly-scanned file(s) "
                          f"under {', '.join(args.adopt_scope)} "
                          f"({total_new} pre-existing assertions):")
                    for path, count, _ in sorted(newly_covered):
                        print(f"  + {path}: {count}")
            if grew:
                print("REFUSING to update the baseline: it may only shrink.\n",
                      file=sys.stderr)
                for path, count, allowed in grew:
                    if allowed is None:
                        print(f"  {path}: {count} raw assert(), not in the "
                              f"current baseline", file=sys.stderr)
                    else:
                        print(f"  {path}: {count} raw assert(), baseline has "
                              f"{allowed} (+{count - allowed})", file=sys.stderr)
                print("\nRemove or convert those assertions first. The baseline "
                      "records the backlog\nbeing paid down; it is not a place "
                      "to record new debt.", file=sys.stderr)
                return 1
        with open(BASELINE_PATH, "w", encoding="utf-8") as fh:
            json.dump(current, fh, indent=2, sort_keys=True)
            fh.write("\n")
        total = sum(current.values())
        was = sum(existing.values()) if existing else None
        delta = f" (was {was})" if was is not None else ""
        print(f"baseline updated: {len(current)} files, "
              f"{total} raw assertions{delta}")
        return 0

    if not existing:
        print(f"ERROR: missing baseline {BASELINE_PATH}", file=sys.stderr)
        print("Generate it with: python3 scripts/ci/check_test_assertions.py "
              "--update-baseline", file=sys.stderr)
        return 2

    baseline = existing

    regressions = []
    for path, count, allowed in find_regressions(current, baseline):
        if allowed is None:
            regressions.append(
                f"  {path}: {count} raw assert() in a file with no baseline entry\n"
                f"      New tests must use always-on checks. assert() is compiled\n"
                f"      out under NDEBUG, so these would not gate in CI.")
        else:
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
    scanned = ", ".join(d.rstrip("/") + "/" for d in SCAN_DIRS)
    print(f"raw assert() under {scanned}: {total_current} across "
          f"{len(current)} files (baseline {total_baseline} across "
          f"{len(baseline)})")
    print(f"NOTE: scope is limited to {scanned} -- compiled test code outside "
          f"these roots is NOT scanned.")

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
