#!/usr/bin/env python3
"""
Quick RocksDB manifest integrity check for Dinero chainstate snapshots.

This tool is meant to catch the exact failure mode we saw on VA/tower:
the active MANIFEST references one or more SST files that are no longer
present on disk, so RocksDB refuses to cold-open the database.

It is intentionally lightweight:
  - no RocksDB bindings
  - no ldb dependency
  - works by reading CURRENT (when present) and scanning the selected
    MANIFEST bytes for "*.sst" and "*.ldb" filenames

Typical usage:
  python3 tools/check_rocksdb_manifest_integrity.py \
    --datadir /root/Dinero-Coin/data-main

  python3 tools/check_rocksdb_manifest_integrity.py \
    --chaindb /home/tower/.dinero/blockchain/chaindb
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


SST_PATTERN = re.compile(rb"(?<!\d)(\d{6}\.(?:sst|ldb))(?!\d)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check that the active RocksDB MANIFEST does not reference missing SST files"
    )
    parser.add_argument(
        "--datadir",
        help="Dinero datadir; chaindb is assumed at <datadir>/blockchain/chaindb",
    )
    parser.add_argument(
        "--chaindb",
        help="Direct path to the RocksDB chaindb directory",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable JSON output",
    )
    parser.add_argument(
        "--strict-current",
        action="store_true",
        help="Fail if CURRENT is missing, malformed, or points to a missing MANIFEST",
    )
    return parser.parse_args()


def choose_chaindb(args: argparse.Namespace) -> Path:
    if bool(args.datadir) == bool(args.chaindb):
        raise SystemExit("Provide exactly one of --datadir or --chaindb")

    if args.chaindb:
        return Path(args.chaindb).expanduser().resolve()

    return (Path(args.datadir).expanduser().resolve() / "blockchain" / "chaindb")


class CurrentFileError(RuntimeError):
    pass


def read_current_manifest_name(chaindb: Path, *, strict: bool) -> Optional[str]:
    current = chaindb / "CURRENT"
    if not current.exists():
        if strict:
            raise CurrentFileError(f"CURRENT file is missing: {current}")
        return None

    name = current.read_text(encoding="utf-8", errors="replace").strip()
    # RocksDB CURRENT usually stores "MANIFEST-xxxxxx\n"
    name = name.splitlines()[0].strip()
    if not name:
        if strict:
            raise CurrentFileError(f"CURRENT file is empty or malformed: {current}")
        return None
    if "/" in name or "\\" in name:
        if strict:
            raise CurrentFileError(f"CURRENT file contains an invalid manifest name: {name}")
        return None
    if not name.startswith("MANIFEST-"):
        if strict:
            raise CurrentFileError(f"CURRENT file does not point to a MANIFEST: {name}")
        return None
    return name


def pick_manifest(chaindb: Path, *, strict_current: bool) -> Tuple[Path, str, Optional[str]]:
    current_name = read_current_manifest_name(chaindb, strict=strict_current)
    if current_name:
        manifest = chaindb / current_name
        if manifest.exists():
            return manifest, "current", current_name
        if strict_current:
            raise CurrentFileError(f"CURRENT points to a missing MANIFEST: {manifest}")

    manifests = sorted(chaindb.glob("MANIFEST-*"))
    if not manifests:
        raise FileNotFoundError(f"No MANIFEST-* files found in {chaindb}")
    if strict_current:
        raise CurrentFileError(f"Failed to resolve manifest from CURRENT in {chaindb}")
    return manifests[-1], "fallback_latest", current_name


def scan_manifest_references(manifest_path: Path) -> List[str]:
    raw = manifest_path.read_bytes()
    refs = {match.decode("ascii") for match in SST_PATTERN.findall(raw)}
    return sorted(refs)


def existing_table_files(chaindb: Path) -> List[str]:
    files = []
    for path in chaindb.iterdir():
        if path.is_file() and path.suffix in {".sst", ".ldb"}:
            files.append(path.name)
    return sorted(files)


def build_report(chaindb: Path, *, strict_current: bool) -> Dict[str, Any]:
    if not chaindb.exists():
        raise FileNotFoundError(f"chaindb directory does not exist: {chaindb}")
    if not chaindb.is_dir():
        raise NotADirectoryError(f"chaindb path is not a directory: {chaindb}")

    manifest, manifest_selection, current_name = pick_manifest(
        chaindb, strict_current=strict_current
    )
    referenced = scan_manifest_references(manifest)
    existing = set(existing_table_files(chaindb))
    missing = [name for name in referenced if name not in existing]
    orphaned = [name for name in sorted(existing) if name not in set(referenced)]

    return {
        "ok": len(missing) == 0,
        "chaindb": str(chaindb),
        "current_file": str(chaindb / "CURRENT"),
        "current_manifest_name": current_name,
        "manifest_path": str(manifest),
        "manifest_selection": manifest_selection,
        "referenced_table_files": referenced,
        "existing_table_files": sorted(existing),
        "missing_table_files": missing,
        "orphaned_table_files": orphaned,
        "referenced_count": len(referenced),
        "existing_count": len(existing),
        "missing_count": len(missing),
        "orphaned_count": len(orphaned),
    }


def print_text_report(report: Dict[str, Any]) -> None:
    print(f"chaindb:   {report['chaindb']}")
    print(f"manifest:  {report['manifest_path']}")
    print(f"selected:  {report['manifest_selection']}")
    print(f"current:   {report['current_manifest_name'] or '(missing CURRENT)'}")
    print(f"refs:      {report['referenced_count']}")
    print(f"existing:  {report['existing_count']}")
    print(f"missing:   {report['missing_count']}")
    print(f"orphaned:  {report['orphaned_count']}")

    if report["missing_table_files"]:
        print("\nMissing table files referenced by the active manifest:")
        for name in report["missing_table_files"]:
            print(f"  - {name}")
    else:
        print("\nNo manifest-referenced SST/ldb files are missing.")

    if report["orphaned_table_files"]:
        print("\nUnreferenced table files present on disk:")
        for name in report["orphaned_table_files"][:20]:
            print(f"  - {name}")
        if report["orphaned_count"] > 20:
            print(f"  ... plus {report['orphaned_count'] - 20} more")


def main() -> int:
    args = parse_args()

    try:
        chaindb = choose_chaindb(args)
        report = build_report(chaindb, strict_current=args.strict_current)
    except Exception as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}, indent=2, sort_keys=True))
        else:
            print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_text_report(report)

    return 0 if report["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
