#!/usr/bin/env python3
"""CV2: live consensus rule-site inventory.

  rule_sites.py <src_root> <rule_sites.tsv>

Each row names one live rule site. Its anchor (a literal string) must occur exactly once in the file.
Exit 1 if any anchor is MISSING / AMBIGUOUS or a file is gone, so the inventory is stale and has to be reviewed.
Exit 2 = bad inventory. On success it prints each site as file:line, the current map of where the rules run.
"""
import csv, os, sys

HEADER = ["rule_id", "file", "function", "anchor", "note"]


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__); return 2
    root, tsv = argv[1:]
    with open(tsv, newline="") as fh:
        rows = list(csv.reader(fh, delimiter="\t", quoting=csv.QUOTE_NONE))
    if not rows or rows[0] != HEADER:
        sys.stderr.write("ERROR: header must be %s\n" % "\\t".join(HEADER)); return 2
    rows = [r for r in rows[1:] if r and not r[0].startswith("#")]
    if not rows:
        sys.stderr.write("ERROR: no rows\n"); return 2
    seen, bad = set(), 0
    for r in rows:
        if len(r) != len(HEADER):
            sys.stderr.write("ERROR: row has %d fields: %r\n" % (len(r), r)); return 2
        rid, path, func, anchor, _ = r
        if rid in seen:
            sys.stderr.write("ERROR: duplicate rule_id %s\n" % rid); return 2
        seen.add(rid)
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            print("NO_FILE %s %s" % (rid, path)); bad += 1; continue
        lines = [i + 1 for i, line in enumerate(open(full, errors="replace")) if anchor in line]
        if not lines:
            print("MISSING %s %s anchor=%r" % (rid, path, anchor)); bad += 1
        elif len(lines) > 1:
            print("AMBIGUOUS %s %s anchor=%r lines=%s" % (rid, path, anchor, lines)); bad += 1
        else:
            print("%s %s:%d %s" % (rid, path, lines[0], func))
    print("CV2 %s: %d sites, %d stale" % ("FAIL" if bad else "PASS", len(rows), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
