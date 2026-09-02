#!/usr/bin/env python3
"""Fail closed when a staged or published release omits required assets."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def names_in(directory: Path) -> set[str]:
    if not directory.is_dir():
        raise SystemExit(f"release asset directory does not exist: {directory}")
    return {entry.name for entry in directory.iterdir() if entry.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("packaging/release-assets-v8.1.11.json"),
    )
    parser.add_argument("--companion-repo")
    parser.add_argument("--companion-component")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.companion_repo:
        matches = [
            item
            for item in manifest["companion_releases"]
            if item["repo"] == args.companion_repo
        ]
        if len(matches) != 1:
            raise SystemExit(
                f"manifest has no unique asset matrix for {args.companion_repo}"
            )
        selected = matches[0]
        if args.companion_component:
            components = [
                item
                for item in selected.get("releases", [])
                if item.get("component") == args.companion_component
            ]
            if len(components) != 1 or "required_assets" not in components[0]:
                raise SystemExit(
                    "manifest has no unique asset matrix for "
                    f"{args.companion_repo}/{args.companion_component}"
                )
            selected = components[0]
        elif "required_assets" not in selected:
            raise SystemExit(
                f"{args.companion_repo} has multiple components; "
                "pass --companion-component"
            )
        required = set(selected["required_assets"])
    else:
        required = set(manifest["required_assets"])

    present = names_in(args.directory)
    missing = sorted(required - present)
    if missing:
        print("FAIL: required release assets are missing:")
        for name in missing:
            print(f"  {name}")
        return 1

    print(f"PASS: all {len(required)} required release assets are present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
