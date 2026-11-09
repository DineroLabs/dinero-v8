#!/usr/bin/env python3
"""Decide whether this PR changed any separation-gate-relevant path.

The eight `gates <toolchain> orchard=<cfg>` contexts are required on
dinero-main. A workflow-level `on.pull_request.paths` filter leaves a
required context that never triggers stuck at "Expected" forever, blocking
every PR outside the filter (docs-only, packaging-only, ...). So the gates
workflows trigger on every PR, this script reproduces the old paths filter
against the PR's changed files, and the heavy jobs are skipped when nothing
relevant changed: a job skipped by its `if:` reports conclusion "skipped",
which GitHub counts as satisfying a required status check.

Fail open: a non-pull_request event, a git error, or an unsupported pattern
means "run the gates" (true). Skipping is only ever an optimization; this
script must never be the reason a gate did not run. (A PR can already edit
the workflow itself on pull_request events, so routing the decision through
a repo script adds no new trust boundary.)

Pattern language (the subset the gates workflows used in on.paths):
  'dir/**'    every file under dir/
  '**/name'   a file named `name` at any depth, the repo root included
  'exact'     that one path
  anything else containing a wildcard: fnmatch (no '**' crossing '/').

Stdlib only. Writes `<key>=true|false` to --github-output and prints the
decision with the matching files. Always exits 0 on a decision (the
workflow's `!cancelled() && != 'false'` condition fails open anyway);
exit 2 only for a usage error.
"""
import argparse
import fnmatch
import subprocess
import sys


def matches(pattern, path):
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-2])
    if pattern.startswith("**/"):
        tail = pattern[3:]
        return path == tail or path.endswith("/" + tail)
    if any(ch in pattern for ch in "*?["):
        return fnmatch.fnmatchcase(path, pattern)
    return path == pattern


def changed_files():
    # actions/checkout on a pull_request event checks out the PR merge
    # commit: parent 1 is the base head, so HEAD^1..HEAD is exactly the
    # PR's effective change (fetch-depth: 2 suffices).
    out = subprocess.run(
        ["git", "diff", "--name-only", "HEAD^1", "HEAD"],
        capture_output=True, text=True, check=True)
    return [line for line in out.stdout.splitlines() if line]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--github-output", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--event", required=True, help="$GITHUB_EVENT_NAME")
    ap.add_argument("patterns", nargs="+")
    args = ap.parse_args()

    relevant, why = True, ""
    if args.event != "pull_request":
        why = f"event {args.event} is not pull_request: run everything"
    else:
        try:
            files = changed_files()
        except (OSError, subprocess.CalledProcessError) as e:
            why = f"git diff failed ({e}): fail open, run everything"
        else:
            hits = [f for f in files
                    if any(matches(p, f) for p in args.patterns)]
            if hits:
                why = "gate-relevant changes:\n  " + "\n  ".join(hits[:50])
            else:
                relevant = False
                why = (f"none of the {len(files)} changed files matches a "
                       "gate path: the gate jobs will report skipped")
    value = "true" if relevant else "false"
    print(f"{args.key}={value}\n{why}")
    with open(args.github_output, "a", encoding="utf-8") as f:
        f.write(f"{args.key}={value}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
