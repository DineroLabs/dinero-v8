#!/usr/bin/env python3
"""Verify that generated first-party compile commands use strict C++20."""

from __future__ import annotations

import argparse
import json
import shlex
from pathlib import Path


CPP_SUFFIXES = {".cc", ".cpp", ".cxx", ".mm"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("compile_commands", type=Path)
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    database = json.loads(args.compile_commands.read_text(encoding="utf-8"))
    violations: list[str] = []
    checked = 0

    for entry in database:
        source = Path(entry["file"]).resolve()
        try:
            relative = source.relative_to(source_root)
        except ValueError:
            continue
        if not relative.parts or relative.parts[0] in {"third_party", "build"}:
            continue
        if any(part.startswith("build-") for part in relative.parts):
            continue
        if source.suffix.lower() not in CPP_SUFFIXES:
            continue

        command = entry.get("arguments")
        if command is None:
            command = shlex.split(entry["command"], posix=True)
        standards = [
            flag
            for flag in command
            if flag.startswith("-std=") or flag.startswith("/std:")
        ]
        checked += 1
        # AppleClang versions supported by the Ventura build use CMake's
        # historical strict spelling `-std=c++2a` for OBJCXX_STANDARD 20.
        # It selects the finalized C++20 mode and, unlike gnu++2a, does not
        # enable compiler extensions.
        if not standards or any(
            standard not in {"-std=c++20", "-std=c++2a", "/std:c++20"}
            for standard in standards
        ):
            violations.append(f"{relative}: {standards or ['<missing>']}")

    if checked == 0:
        print("ERROR: no first-party C++ compile commands were found")
        return 1
    if violations:
        print("ERROR: first-party compile commands are not strict C++20:")
        for violation in violations:
            print(f"  {violation}")
        return 1

    print(f"PASS: {checked} first-party C++ compile commands use strict C++20")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
