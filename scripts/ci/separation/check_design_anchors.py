#!/usr/bin/env python3
"""Check design anchors: usage check_design_anchors.py <repo> <tsv>.

TSV rows: path<TAB>line(1-based)<TAB>expected substring. Lines starting '#'
and blank lines are skipped. Prints 'MOVED path old->new' (substring found
within +-200 lines, nearest first) or 'MISSING path:line'. Exit 1 on any.
"""
import sys
from pathlib import Path

WINDOW = 200


def check(repo, tsv):
    bad = False
    for raw in Path(tsv).read_text().splitlines():
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = raw.split("\t", 2)
        if len(parts) != 3:
            print(f"MALFORMED {raw!r}")
            bad = True
            continue
        path, line, sub = parts[0], int(parts[1]), parts[2]
        try:
            lines = (Path(repo) / path).read_text(errors="replace").splitlines()
        except OSError:
            print(f"MISSING {path}:{line}")
            bad = True
            continue
        if 1 <= line <= len(lines) and sub in lines[line - 1]:
            continue
        bad = True
        found = None
        for d in range(1, WINDOW + 1):
            for cand in (line - d, line + d):
                if 1 <= cand <= len(lines) and sub in lines[cand - 1]:
                    found = cand
                    break
            if found:
                break
        print(f"MOVED {path} {line}->{found}" if found else f"MISSING {path}:{line}")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_design_anchors.py <repo> <tsv>")
    sys.exit(check(sys.argv[1], sys.argv[2]))
