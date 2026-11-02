#!/usr/bin/env python3
"""Capture the selected H149999 v5 state before Orchard activates.

This only stages the existing publisher's pending artifact. The publisher must
still wait for 288 descendants and pass its native import gate before serving
anything. Start ``--watch`` before H149999 with the daily publisher held.
"""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

from snapshot_transport import inspect

BOUNDARY_HEIGHT = 149999


def sync_directory(path: Path) -> None:
    directory = os.open(path, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def rpc(datadir: Path, *args: str) -> str:
    cli = os.environ.get("CLI_BIN", "dinero-cli")
    return subprocess.check_output(
        [cli, f"-datadir={datadir}", *args], text=True, stderr=subprocess.PIPE
    ).strip()


def count(datadir: Path) -> int:
    value = json.loads(rpc(datadir, "getblockcount"))
    if type(value) is not int or value < 0:
        raise ValueError("invalid selected tip height")
    return value


def selected_hash(datadir: Path, height: int) -> str:
    value = json.loads(rpc(datadir, "getblockhash", str(height)))
    if not isinstance(value, str) or len(value) != 64:
        raise ValueError("invalid selected block hash")
    int(value, 16)
    return value.lower()


def verify_existing(datadir: Path, dumps: Path, height: int) -> bool:
    payload, metadata = dumps / "pending.dat", dumps / "pending.json"
    if not payload.exists() and not metadata.exists():
        return False
    if payload.is_file() and not metadata.exists():
        # An interruption after the payload rename leaves the only exact-height
        # export without its metadata. Recover it from its native header and
        # selected-chain identity; the publisher's native loader remains the
        # authority before publication.
        version, base_height, base_hash, profile = inspect(payload)
        if (version, base_height, profile) != (5, height, "legacy") or \
                selected_hash(datadir, height) != base_hash:
            raise ValueError("unpaired payload is not the selected H149999 v5 base")
        digest = hashlib.sha256()
        with payload.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        record = {
            "base_height": height, "base_hash": base_hash,
            "format_version": 5, "bytes": payload.stat().st_size,
            "sha256": digest.hexdigest(), "boundary_capture": True,
        }
        temporary = dumps / f".pending-recovery-{os.getpid()}-{uuid.uuid4().hex}.json"
        try:
            with temporary.open("x") as stream:
                json.dump(record, stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, metadata)
            sync_directory(dumps)
        finally:
            temporary.unlink(missing_ok=True)
        print(f"recovered v5 base={height} hash={base_hash} from retained payload", flush=True)
        return True
    if not payload.is_file() or not metadata.is_file():
        raise ValueError("partial pending artifact exists; inspect before retry")
    data = json.loads(metadata.read_text())
    version, base_height, base_hash, profile = inspect(payload)
    if (version, base_height, profile) != (5, height, "legacy") or data.get("base_hash") != base_hash or data.get("base_height") != height or data.get("boundary_capture") is not True:
        raise ValueError("a different publisher candidate is pending")
    digest = hashlib.sha256()
    with payload.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if data.get("bytes") != payload.stat().st_size or data.get("sha256") != digest.hexdigest():
        raise ValueError("existing boundary candidate checksum differs")
    if selected_hash(datadir, height) != base_hash:
        raise ValueError("existing boundary candidate left the selected chain")
    print(f"already captured v5 base={height} hash={base_hash}", flush=True)
    return True


def capture(datadir: Path, height: int) -> None:
    dumps = datadir / "snapshots"
    dumps.mkdir(parents=True, exist_ok=True)
    lock = os.open(dumps / "publisher.lock", os.O_WRONLY | os.O_CREAT, 0o600)
    try:
        # The ordinary publisher and this exact-height capture may never race.
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if verify_existing(datadir, dumps, height):
            return
        if count(datadir) != height:
            raise ValueError(f"selected tip is not exact boundary height {height}")
        before = selected_hash(datadir, height)
        temporary = dumps / f".boundary-v5-{height}-{os.getpid()}-{uuid.uuid4().hex}.dat"
        metadata_tmp = dumps / f".{temporary.name}.json"
        try:
            output = json.loads(rpc(datadir, "dumptxoutset", str(temporary), "compact"))
            version, base_height, base_hash, profile = inspect(temporary)
            if (version, base_height, base_hash, profile) != (5, height, before, "legacy"):
                raise ValueError("exported snapshot is not the selected exact-height v5 base")
            if type(output.get("base_height")) is not int or output["base_height"] != height or not isinstance(output.get("base_hash"), str) or output["base_hash"].lower() != before:
                raise ValueError("export RPC and snapshot header disagree")
            if selected_hash(datadir, height) != before:
                raise ValueError("boundary base left selected chain during export")
            digest = hashlib.sha256()
            with temporary.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            record = {
                "base_height": height,
                "base_hash": before,
                "format_version": 5,
                "bytes": temporary.stat().st_size,
                "sha256": digest.hexdigest(),
                "boundary_capture": True,
            }
            with metadata_tmp.open("x") as stream:
                json.dump(record, stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            with temporary.open("rb") as stream:
                os.fsync(stream.fileno())
            os.replace(temporary, dumps / "pending.dat")
            # Make the data rename durable before installing metadata. After
            # power loss, the recoverable state is data-only or the full pair.
            sync_directory(dumps)
            os.replace(metadata_tmp, dumps / "pending.json")
            sync_directory(dumps)
            print(f"captured v5 base={height} hash={before} sha256={record['sha256']}; awaiting 288 descendants and native publisher gate", flush=True)
        finally:
            temporary.unlink(missing_ok=True)
            metadata_tmp.unlink(missing_ok=True)
    finally:
        os.close(lock)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watch", action="store_true", help="poll until the exact height, then capture; fail if missed")
    args = parser.parse_args()
    datadir = Path(os.environ.get("DATADIR", "/var/lib/dinero"))
    try:
        chain = json.loads(rpc(datadir, "getblockchaininfo"))
        if chain.get("chain") != "mainnet":
            raise ValueError("exact H149999 capture requires mainnet")
        while True:
            tip = count(datadir)
            if tip >= BOUNDARY_HEIGHT:
                # A completed or data-only capture remains valid after the tip
                # advances. Only a new export is forbidden after H149999.
                dumps = datadir / "snapshots"
                if tip > BOUNDARY_HEIGHT and not (dumps / "pending.dat").exists() and \
                        not (dumps / "pending.json").exists():
                    raise ValueError(f"missed H{BOUNDARY_HEIGHT}; current selected tip is H{tip}")
                capture(datadir, BOUNDARY_HEIGHT)
                return 0
            if not args.watch:
                raise ValueError(f"selected tip H{tip} is below H{BOUNDARY_HEIGHT}; start --watch before the boundary")
            time.sleep(float(os.environ.get("SNAPSHOT_BOUNDARY_POLL_SECONDS", "1")))
    except (OSError, ValueError, subprocess.CalledProcessError, BlockingIOError) as error:
        print(f"boundary capture refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
