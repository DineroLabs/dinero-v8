#!/usr/bin/env python3
"""GATE P1: ambient-global reads in the consensus libraries may only shrink.

docs/superpowers/specs/2026-11-10-consensus-globals-purity-design.md §4.

Scans the sources of the two consensus library targets (parsed from
src/consensus/CMakeLists.txt, never hardcoded) plus the extra files named in
p1_rules.json for ambient-state tokens (chain-selection accessors, singleton
accessors, getenv, wall clock, the weak ParamsImpl symbol). Counts are
compared with the committed baseline consensus_globals_baseline.tsv:

  - a count ABOVE baseline (or a new row)  -> FAIL: regression.
  - a count BELOW baseline (or a gone row) -> FAIL: stale baseline; the
    commit that lowers a count must regenerate the baseline in the same
    commit (--write-baseline), which is what makes the ratchet atomic.
  - exact match                            -> PASS.

Tiers: K = dinero_consensus_core sources ("the kernel", end state: empty
baseline), L = dinero_consensus sources not already in K. A file present in
both targets is reported once, under K (the stricter tier).

Exempt files (p1_rules.json): the chain-parameters implementation itself —
the gate polices consumers of ambient state, not the one definition site.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RULES_PATH = HERE / "p1_rules.json"
BASELINE_PATH = HERE / "consensus_globals_baseline.tsv"
CMAKE_REL = Path("src/consensus/CMakeLists.txt")

TARGETS = ("dinero_consensus_core", "dinero_consensus")
TIER_OF = {"dinero_consensus_core": "K", "dinero_consensus": "L"}


def load_rules(path=RULES_PATH):
    rules = json.loads(Path(path).read_text(encoding="utf-8"))
    rules["_compiled"] = [(name, re.compile(pat)) for name, pat in rules["tokens"].items()]
    return rules


def parse_cmake_sets(repo, cmake_rel=CMAKE_REL):
    """Return {target: [source paths]} from the add_library blocks."""
    text = (Path(repo) / cmake_rel).read_text(encoding="utf-8")
    out = {}
    for target in TARGETS:
        m = re.search(r"add_library\(\s*" + re.escape(target) + r"\s+STATIC\b", text)
        if not m:
            raise SystemExit(f"GATE P1 ERROR: add_library({target} STATIC ...) not found in {cmake_rel}")
        depth, sources, pos = 0, [], m.start()
        for line in text[pos:].splitlines():
            code = line.split("#", 1)[0]
            depth += code.count("(") - code.count(")")
            sources += [w for w in code.replace("(", " ").replace(")", " ").split()
                        if w.endswith((".cpp", ".c", ".cc"))]
            if depth <= 0:
                break
        out[target] = sources
    return out


def strip_comments_and_strings(text):
    """Blank out comments and string/char literal contents (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n - 2 if j == -1 else j
            seg = text[i:j + 2]
            out.append("".join(ch if ch == "\n" else " " for ch in seg))
            i = j + 2
        elif c in "\"'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            seg = text[i:min(j + 1, n)]
            out.append(c + "".join(ch if ch == "\n" else " " for ch in seg[1:-1]) + (q if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan_file(path, compiled_tokens):
    text = strip_comments_and_strings(Path(path).read_text(encoding="utf-8", errors="replace"))
    counts = {}
    for name, rx in compiled_tokens:
        k = len(rx.findall(text))
        if k:
            counts[name] = k
    return counts


def scan_repo(repo, rules=None):
    """Return sorted rows [(tier, relpath, token, count)] for the live tree."""
    repo = Path(repo)
    rules = rules or load_rules()
    sets = parse_cmake_sets(repo)
    exempt = set(rules.get("exempt_files", []))
    tier_files = {}
    for target in TARGETS:  # K first, so L drops duplicates
        tier = TIER_OF[target]
        for rel in sets[target] + rules.get("extra_files", {}).get(tier, []):
            if rel in exempt or any(rel in v for v in tier_files.values()):
                continue
            tier_files.setdefault(tier, []).append(rel)
    rows = []
    for tier, rels in tier_files.items():
        for rel in rels:
            p = repo / rel
            if not p.is_file():
                raise SystemExit(f"GATE P1 ERROR: listed source missing: {rel}")
            for token, count in scan_file(p, rules["_compiled"]).items():
                rows.append((tier, rel, token, count))
    return sorted(rows)


def read_baseline(path=BASELINE_PATH):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tier, rel, token, count = line.split("\t")
        rows.append((tier, rel, token, int(count)))
    return sorted(rows)


def write_baseline(rows, path=BASELINE_PATH):
    lines = ["# GATE P1 baseline: tier<TAB>file<TAB>token<TAB>count. Regenerate with",
             "# scripts/ci/separation/check_consensus_globals.py --write-baseline"]
    lines += [f"{t}\t{f}\t{tok}\t{c}" for t, f, tok, c in rows]
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def compare(current, baseline):
    """Return (regressions, stale) row lists; both empty means PASS."""
    cur = {(t, f, tok): c for t, f, tok, c in current}
    base = {(t, f, tok): c for t, f, tok, c in baseline}
    regressions = [(k, base.get(k, 0), c) for k, c in sorted(cur.items()) if c > base.get(k, 0)]
    stale = [(k, b, cur.get(k, 0)) for k, b in sorted(base.items()) if cur.get(k, 0) < b]
    return regressions, stale


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=str(HERE.parents[2]))
    ap.add_argument("--write-baseline", action="store_true")
    args = ap.parse_args(argv)

    current = scan_repo(args.repo)
    if args.write_baseline:
        write_baseline(current)
        print(f"GATE P1 baseline written: {len(current)} rows")
        return 0

    regressions, stale = compare(current, read_baseline())
    for (tier, rel, token), b, c in regressions:
        print(f"P1 REGRESSION [{tier}] {rel}: {token} {b} -> {c}")
    for (tier, rel, token), b, c in stale:
        print(f"P1 STALE BASELINE [{tier}] {rel}: {token} {b} -> {c} "
              f"(improvement not recorded; rerun with --write-baseline in the same commit)")
    if regressions or stale:
        print(f"GATE P1 FAIL rc=1 ({len(regressions)} regression row(s), {len(stale)} stale row(s))")
        return 1
    k_rows = sum(1 for t, *_ in current if t == "K")
    print(f"GATE P1 PASS ({len(current)} baseline rows, {k_rows} in tier K)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
