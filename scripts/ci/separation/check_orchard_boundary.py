#!/usr/bin/env python3
"""GATE O1: the free-standing Orchard consensus clusters stay free-standing.

docs/superpowers/specs/2026-11-11-consensus-orchard-separation-design.md §4.

Two tiers, both measured from a built Orchard-ON tree (--build):

Symbol tier: for each gated archive (o1_rules.json "gated_archives"), every
undefined symbol must NOT be defined by a deny-listed definer archive
("definer_archives": dinero_core, dinero_chainstate, dinero_shielded). A hit
means the cluster grew a dependency on node/storage/shielded code.

Header tier: for each object of a gated archive, the headers it was built
against (ninja -t deps) must not reach the forbidden families
("forbidden_header_prefixes": storage/daemon/wallet/mempool). Known
pre-existing reaches live in the committed baseline and may only shrink.

Baseline semantics are P1's, strict both ways: a count above baseline (or a
new row) fails as a regression; a count below baseline fails as stale until
--write-baseline runs in the same change. Symbol-tier end state is an empty
baseline (zero tolerance); header-tier rows ratchet down as the interface
cleanup lands.

Exit: 0 PASS, 1 violations/stale, 2 input error (missing build artifacts).
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RULES_PATH = HERE / "o1_rules.json"
BASELINE_PATH = HERE / "orchard_boundary_baseline.tsv"


def load_rules(path=RULES_PATH):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def find_archive(build, name):
    hits = sorted(Path(build).rglob(name))
    if not hits:
        raise SystemExit(f"GATE O1 ERROR rc=2: archive not found under {build}: {name}")
    return hits[0]


def nm_lines(nm, args, path):
    out = subprocess.run([nm, *args, str(path)], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"GATE O1 ERROR rc=2: nm failed on {path}: {out.stderr.strip()[:200]}")
    return out.stdout.splitlines()


def defined_symbols(nm, path):
    syms = set()
    for line in nm_lines(nm, ["-A", "--defined-only"], path):
        parts = line.split()
        if parts and not parts[-1].startswith("."):
            syms.add(parts[-1])
    return syms


def undefined_symbols(nm, path):
    syms = set()
    for line in nm_lines(nm, ["-A", "-u"], path):
        parts = line.split()
        if parts:
            syms.add(parts[-1])
    return syms


def object_deps(ninja, build, obj):
    out = subprocess.run([ninja, "-C", str(build), "-t", "deps", obj],
                         capture_output=True, text=True)
    deps = []
    for line in out.stdout.splitlines():
        line = line.strip()
        if line and not line.endswith(":") and "deps mtime" not in line:
            deps.append(line)
    return deps


def gated_objects(build, archive_path):
    out = subprocess.run(["ar", "t", str(archive_path)], capture_output=True, text=True)
    names = [n for n in out.stdout.split() if n.endswith(".o")]
    # ninja deps keys are build-relative object paths; find them by basename.
    index = {}
    for p in Path(build).rglob("*.o"):
        index.setdefault(p.name, []).append(p.relative_to(build))
    objs = []
    for n in names:
        for rel in index.get(n, []):
            objs.append(str(rel))
    return objs


def scan(build, rules, nm="nm", ninja="ninja"):
    """Return sorted rows [(tier, archive-or-source, token, count)]."""
    build = Path(build)
    rows = []

    definers = {}
    for name in rules["definer_archives"]:
        definers[name] = defined_symbols(nm, find_archive(build, name))

    forbidden = tuple(rules["forbidden_header_prefixes"])
    for name in rules["gated_archives"]:
        arch = find_archive(build, name)
        undef = undefined_symbols(nm, arch)
        for dname, dsyms in definers.items():
            hits = undef & dsyms
            if hits:
                rows.append(("S", name, dname, len(hits)))
        per_source = {}
        for obj in gated_objects(build, arch):
            for dep in object_deps(ninja, build, obj):
                norm = dep.lstrip("./")
                # deps are relative to the build dir; repo files come through
                # as ../<repo-relative> or absolute — normalize to repo-relative
                norm = re.sub(r"^(\.\./)+", "", norm)
                for pref in forbidden:
                    if pref in norm:
                        src = Path(obj).name.replace(".cpp.o", ".cpp").replace(".c.o", ".c")
                        key = (src, pref)
                        per_source[key] = per_source.get(key, set())
                        per_source[key].add(norm)
        for (src, pref), headers in sorted(per_source.items()):
            rows.append(("H", src, pref, len(headers)))
    return sorted(rows)


def read_baseline(path=BASELINE_PATH):
    rows = []
    p = Path(path)
    if not p.exists():
        return rows
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tier, a, token, count = line.split("\t")
        rows.append((tier, a, token, int(count)))
    return sorted(rows)


def write_baseline(rows, path=BASELINE_PATH):
    lines = ["# GATE O1 baseline: tier<TAB>archive-or-source<TAB>definer-or-family<TAB>count.",
             "# Regenerate with check_orchard_boundary.py --build <orchard-ON build> --write-baseline"]
    lines += [f"{t}\t{a}\t{tok}\t{c}" for t, a, tok, c in rows]
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def compare(current, baseline):
    cur = {(t, a, tok): c for t, a, tok, c in current}
    base = {(t, a, tok): c for t, a, tok, c in baseline}
    regressions = [(k, base.get(k, 0), c) for k, c in sorted(cur.items()) if c > base.get(k, 0)]
    stale = [(k, b, cur.get(k, 0)) for k, b in sorted(base.items()) if cur.get(k, 0) < b]
    return regressions, stale


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--ninja", default="ninja")
    ap.add_argument("--write-baseline", action="store_true")
    args = ap.parse_args(argv)

    current = scan(args.build, load_rules(), nm=args.nm, ninja=args.ninja)
    if args.write_baseline:
        write_baseline(current)
        print(f"GATE O1 baseline written: {len(current)} rows")
        return 0

    regressions, stale = compare(current, read_baseline())
    for (tier, a, tok), b, c in regressions:
        print(f"O1 REGRESSION [{tier}] {a}: {tok} {b} -> {c}")
    for (tier, a, tok), b, c in stale:
        print(f"O1 STALE BASELINE [{tier}] {a}: {tok} {b} -> {c} "
              f"(improvement not recorded; rerun with --write-baseline in the same change)")
    if regressions or stale:
        print(f"GATE O1 FAIL rc=1 ({len(regressions)} regression row(s), {len(stale)} stale row(s))")
        return 1
    s_rows = sum(1 for t, *_ in current if t == "S")
    print(f"GATE O1 PASS ({len(current)} baseline rows, {s_rows} symbol-tier)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
