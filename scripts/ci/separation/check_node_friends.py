#!/usr/bin/env python3
"""GATE F1: no node-side class appears in a friend declaration in wallet code.

Scans include/wallet/** and src/wallet/** for `friend class|struct X;` where X
(unqualified or namespace-qualified) is on the node-side deny list. The wallet
may befriend its own classes; it may never befriend node code — that is the
boundary the wallet/node separation established, and this gate keeps it.

Spec: docs/superpowers/specs/2026-11-09-node-friend-access-removal-design.md §4.

Exit 0: GATE F1 PASS. Exit 1: GATE F1 FAIL, one line per finding.
"""
from __future__ import annotations

import pathlib
import re
import sys

# Classes defined in node code (dinero_core and friends). Extend as needed.
NODE_SIDE_CLASSES = ("ChainstateService",)

WALLET_ROOTS = ("include/wallet", "src/wallet")
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx"}

FRIEND_RE = re.compile(
    r"^\s*friend\s+(?:class|struct)\s+(?:[A-Za-z_][A-Za-z0-9_]*::)*([A-Za-z_][A-Za-z0-9_]*)\s*;"
)


def scan_text(text: str, rel_name: str) -> list[str]:
    """Return findings for one file's contents."""
    findings = []
    for lineno, line in enumerate(text.splitlines(), 1):
        m = FRIEND_RE.match(line)
        if m and m.group(1) in NODE_SIDE_CLASSES:
            findings.append(f"{rel_name}:{lineno} grants {m.group(1)}")
    return findings


def scan_repo(repo: pathlib.Path) -> list[str]:
    findings = []
    for root in WALLET_ROOTS:
        base = repo / root
        if not base.is_dir():
            raise SystemExit(f"GATE F1 FAIL rc=2 (missing wallet root {root})")
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            findings.extend(scan_text(text, str(path.relative_to(repo))))
    return findings


def main(argv: list[str]) -> int:
    repo = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path.cwd()
    findings = scan_repo(repo)
    if findings:
        for finding in findings:
            print(finding)
        print(f"GATE F1 FAIL rc=1 ({len(findings)} node-side friend grant(s) in wallet code)")
        return 1
    print("GATE F1 PASS (no node-side friend grants in wallet code)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
