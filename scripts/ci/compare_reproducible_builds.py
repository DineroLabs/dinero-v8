#!/usr/bin/env python3
"""Fail unless two independently built artifacts are byte-identical."""

from __future__ import annotations

import hashlib
import pathlib
import sys


def digest(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as artifact:
        for chunk in iter(lambda: artifact.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} ARTIFACT_A ARTIFACT_B", file=sys.stderr)
        return 2

    first, second = map(pathlib.Path, sys.argv[1:])
    for path in (first, second):
        if not path.is_file():
            print(f"missing artifact: {path}", file=sys.stderr)
            return 2

    first_hash = digest(first)
    second_hash = digest(second)
    print(f"{first_hash}  {first}")
    print(f"{second_hash}  {second}")
    if first_hash != second_hash or first.read_bytes() != second.read_bytes():
        print("reproducibility failure: artifacts differ", file=sys.stderr)
        return 1

    print("reproducibility verified: artifacts are byte-identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
