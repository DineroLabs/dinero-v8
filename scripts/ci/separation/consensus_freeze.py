#!/usr/bin/env python3
"""Consensus source freeze (K5, spec sections 6.1/6.5/6.6/8).

usage: consensus_freeze.py record --repo . --commit REV --baseline consensus_baseline.sha256
       consensus_freeze.py check  --repo . --baseline consensus_baseline.sha256 \
                                  --allowed consensus_allowed_changes.txt --stage N

The repo's own CONSENSUS_MANIFEST (include/consensus/freeze.h) is stale and never
verified; this is the independent guard. `record` hashes (sha256) every file
under src/consensus/** and include/consensus/** as of git commit REV and writes

  # commit <full sha>
  <sha256>  <path>

Baseline reproduction (committed file was made with):
  python3 scripts/ci/separation/consensus_freeze.py record --repo . \
      --commit 1667f8ed6 --baseline scripts/ci/separation/consensus_baseline.sha256

`check` compares the working tree to the baseline and prints, per violation:
  CHANGED path | ADDED path | REMOVED path | HUNK path:line <text>
A difference is allowed only if consensus_allowed_changes.txt has an entry for a
stage <= N (union). Allowed-file lines ('#' comments, blank lines ignored):
  <stage> modify <path>            any change to an existing file
  <stage> remove <path>            the file may be deleted
  <stage> add <path>               the file may be added
  <stage> hunk <path> <regex>      file may change, but ONLY on diff lines (removed
                                   or added, vs the baseline blob) matching one of
                                   its hunk regexes (re.search); hunk entries of
                                   all stages <= N are unioned.
Exit 0 clean, 1 violations, 2 input errors.
"""
import argparse
import difflib
import hashlib
import os
import re
import subprocess
import sys

ROOTS = ("src/consensus", "include/consensus")
IGNORE = {".DS_Store"}


class InputError(Exception):
    pass


def git_bytes(repo, *args):
    try:
        r = subprocess.run(["git", *args], cwd=repo, capture_output=True)
    except OSError as e:
        raise InputError("cannot run git: %s" % e)
    if r.returncode != 0:
        raise InputError("git %s failed: %s" % (" ".join(args),
                                                r.stderr.decode(errors="replace").strip()))
    return r.stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def record(args):
    sha_full = git_bytes(args.repo, "rev-parse", "--verify", args.commit + "^{commit}").decode().strip()
    names = git_bytes(args.repo, "ls-tree", "-r", "-z", "--name-only", sha_full, "--", *ROOTS)
    paths = sorted(p for p in names.decode().split("\0") if p)
    if not paths:
        raise InputError("no files under %s at %s" % (ROOTS, sha_full))
    lines = ["# commit %s" % sha_full]
    for p in paths:
        lines.append("%s  %s" % (sha(git_bytes(args.repo, "cat-file", "blob", "%s:%s" % (sha_full, p))), p))
    with open(args.baseline, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("recorded %d files at %s" % (len(paths), sha_full))
    return 0


def load_baseline(path):
    commit, files = None, {}
    try:
        text = open(path).read()
    except OSError as e:
        raise InputError("cannot read baseline %s: %s" % (path, e))
    for n, line in enumerate(text.splitlines(), 1):
        if line.startswith("# commit "):
            commit = line.split()[2]
        elif line.strip() and not line.startswith("#"):
            m = re.match(r"^([0-9a-f]{64})  (\S.*)$", line)
            if not m:
                raise InputError("%s:%d: bad baseline line" % (path, n))
            files[m.group(2)] = m.group(1)
    if not commit or not files:
        raise InputError("baseline %s has no commit header or no files" % path)
    return commit, files


def load_allowed(path, stage):
    modify, remove, add, hunks = set(), set(), set(), {}
    try:
        text = open(path).read()
    except OSError as e:
        raise InputError("cannot read allowed list %s: %s" % (path, e))
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split(None, 3)
        try:
            st, kind, p = int(parts[0]), parts[1], parts[2]
        except (ValueError, IndexError):
            raise InputError("%s:%d: bad entry" % (path, n))
        if kind not in ("modify", "remove", "add", "hunk"):
            raise InputError("%s:%d: unknown kind %r" % (path, n, kind))
        if kind == "hunk":
            if len(parts) < 4:
                raise InputError("%s:%d: hunk needs a regex" % (path, n))
            try:
                rx = re.compile(parts[3].strip())
            except re.error as e:
                raise InputError("%s:%d: bad regex: %s" % (path, n, e))
        if st > stage:
            continue
        if kind == "modify":
            modify.add(p)
        elif kind == "remove":
            remove.add(p)
        elif kind == "add":
            add.add(p)
        else:
            hunks.setdefault(p, []).append(rx)
    return modify, remove, add, hunks


def tree_files(repo):
    out = {}
    for root in ROOTS:
        for d, _, fs in os.walk(os.path.join(repo, root)):
            for f in fs:
                if f in IGNORE:
                    continue
                full = os.path.join(d, f)
                out[os.path.relpath(full, repo)] = full
    return out


def changed_lines(old, new):
    """Yield (lineno, text) for each removed ('-', old numbering) / added ('+', new numbering) line."""
    a = old.decode(errors="replace").splitlines()
    b = new.decode(errors="replace").splitlines()
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        for i in range(i1, i2):
            yield i + 1, "-" + a[i]
        for j in range(j1, j2):
            yield j + 1, "+" + b[j]


def check(args):
    commit, base = load_baseline(args.baseline)
    modify, remove, add, hunks = load_allowed(args.allowed, args.stage)
    cur = tree_files(args.repo)
    bad = []
    for p in sorted(set(base) | set(cur)):
        if p not in cur:
            if p not in remove:
                bad.append("REMOVED %s" % p)
        elif p not in base:
            if p not in add:
                bad.append("ADDED %s" % p)
        else:
            data = open(cur[p], "rb").read()
            if sha(data) == base[p] or p in modify:
                continue
            if p not in hunks:
                bad.append("CHANGED %s" % p)
                continue
            old = git_bytes(args.repo, "cat-file", "blob", "%s:%s" % (commit, p))
            if sha(old) != base[p]:
                raise InputError("baseline blob for %s at %s does not match recorded hash" % (p, commit))
            for ln, text in changed_lines(old, data):
                if not any(rx.search(text[1:]) for rx in hunks[p]):
                    bad.append("HUNK %s:%d %s" % (p, ln, text))
    for b in bad:
        print(b)
    if bad:
        print("consensus_freeze: %d violation(s) at stage %d" % (len(bad), args.stage))
        return 1
    print("# consensus_freeze: PASS (%d files, stage %d, baseline %s)" % (len(base), args.stage, commit[:9]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--repo", default=".")
    r.add_argument("--commit", required=True)
    r.add_argument("--baseline", required=True)
    c = sub.add_parser("check")
    c.add_argument("--repo", default=".")
    c.add_argument("--baseline", required=True)
    c.add_argument("--allowed", required=True)
    c.add_argument("--stage", type=int, required=True)
    args = ap.parse_args()
    try:
        return record(args) if args.cmd == "record" else check(args)
    except InputError as e:
        print("consensus_freeze: error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
