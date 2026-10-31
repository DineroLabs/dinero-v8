#!/usr/bin/env python3
"""Copy only harness receipts/stdout, never node datadirs, for artifact upload."""
import argparse
import os
from pathlib import Path
import shutil
import stat

CASES = (
    "DEFAULT", "DIRECT_QUIC", "DIALBACK", "COMMUNITY_RELAY", "TWO_RELAYS",
    "PUNCH_V4", "PUNCH_V6", "PUNCH_SYMMETRIC", "PUNCH_V4_LOSS",
    "PUNCH_V6_LOSS", "SHAPED_RELAY",
)
NODE_LOGS = (
    "origin", "relay", "target", "p1", "p2", "p3", "p4", "e", "d",
    "r", "e1", "e2",
)


def regular_receipt(source, relative):
    """Refuse symlinks at every component, without traversing node datadirs."""
    current = source
    parts = (Path("."), *relative.parts)
    for index, part in enumerate(parts):
        current = current / part
        try:
            mode = current.lstat().st_mode
        except FileNotFoundError:
            return False
        expected = stat.S_ISREG if index == len(parts) - 1 else stat.S_ISDIR
        if not expected(mode):
            return False
    return True


def collect(source, destination, uid, gid):
    destination.mkdir(mode=0o700, parents=True, exist_ok=False)
    receipts = [Path("summary.txt"), Path("source-sha.txt")]
    for case in CASES:
        receipts.append(Path(case) / "test.log")
        receipts.extend(Path(case) / "nodes" / f"{name}.log" for name in NODE_LOGS)
    copied = 0
    for relative in receipts:
        if not regular_receipt(source, relative):
            continue
        target = destination / relative
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        shutil.copyfile(source / relative, target)
        target.chmod(0o600)
        copied += 1
    # Only the new allowlisted copy changes ownership. Live/test wallet
    # directories and their 0700 permissions remain untouched.
    for target in [*destination.rglob("*"), destination]:
        os.chown(target, uid, gid)
    return copied


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--uid", type=int, default=os.getuid())
    parser.add_argument("--gid", type=int, default=os.getgid())
    args = parser.parse_args()
    count = collect(args.source, args.destination, args.uid, args.gid)
    print(f"Collected {count} allowlisted relay evidence files; no datadirs copied")


if __name__ == "__main__":
    main()
