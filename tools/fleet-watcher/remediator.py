#!/usr/bin/env python3
"""Root-side, fixed-purpose Dinero restart helper.

The unprivileged watcher can request exactly one action: restart its local
`dinero.service` once for a confirmed `node_behind` incident. Receipts are
written before the restart, deliberately preferring at-most-once behavior over
a restart loop if the service manager call fails or the host crashes.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
import stat
import time


INCIDENT_RE = re.compile(r"^[0-9a-f]{32}$")
EXPECTED_KEYS = {"version", "incident_id", "node", "rule", "requested_at"}


def _read_request(path: str) -> dict:
    # O_NONBLOCK: opening a FIFO planted at this path would otherwise block
    # this root helper forever (DoS of the remediation service). With
    # O_NONBLOCK the open returns immediately and fstat unmasks the fake.
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | os.O_NONBLOCK
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("request is not a regular file")
        if info.st_size > 4096:
            raise ValueError("request exceeds 4096 bytes")
        if info.st_mode & 0o022:
            raise ValueError("request is group/other writable")
        data = os.read(fd, 4097)
    finally:
        os.close(fd)
    if len(data) > 4096:
        raise ValueError("request exceeds 4096 bytes")
    payload = json.loads(data.decode("utf-8"))
    if not isinstance(payload, dict) or set(payload) != EXPECTED_KEYS:
        raise ValueError("request has unexpected shape")
    return payload


def _validate(payload: dict, local_node: str, now: float,
              max_request_age: float) -> str:
    incident_id = payload.get("incident_id")
    if payload.get("version") != 1:
        raise ValueError("unsupported request version")
    if payload.get("rule") != "node_behind":
        raise ValueError("only node_behind may be remediated")
    if payload.get("node") != local_node:
        raise ValueError("request is not for this local node")
    if not isinstance(incident_id, str) or not INCIDENT_RE.fullmatch(incident_id):
        raise ValueError("invalid incident id")
    requested_at = payload.get("requested_at")
    if (isinstance(requested_at, bool)
            or not isinstance(requested_at, (int, float))
            or not math.isfinite(requested_at)):
        raise ValueError("invalid request timestamp")
    age = now - requested_at
    if age > max_request_age or age < -60:
        raise ValueError("stale or future-dated request")
    return incident_id


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True)
    parser.add_argument("--receipts", required=True)
    parser.add_argument("--local-node", required=True)
    parser.add_argument("--service", default="dinero.service")
    parser.add_argument("--systemctl", default="/usr/bin/systemctl")
    parser.add_argument("--max-request-age", type=float, default=300.0)
    args = parser.parse_args(argv)

    try:
        payload = _read_request(args.request)
        incident_id = _validate(
            payload, args.local_node, time.time(), args.max_request_age)
    except Exception as exc:  # invalid input must not retrigger the path forever
        print(f"[remediator] rejected request: {exc}", file=sys.stderr)
        try:
            os.unlink(args.request)
        except FileNotFoundError:
            pass
        return 2

    os.makedirs(args.receipts, mode=0o700, exist_ok=True)
    receipt = os.path.join(args.receipts, incident_id)
    try:
        fd = os.open(receipt, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        os.unlink(args.request)
        print(f"[remediator] incident {incident_id} already handled; no restart")
        return 0

    # Commit the at-most-once decision before touching the daemon. If restart
    # fails, the alert remains open for an operator; it is never retried into a
    # loop by an automatic component.
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(f"requested_at={payload['requested_at']}\n")
        handle.write(f"accepted_at={time.time()}\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.unlink(args.request)

    result = subprocess.run(
        [args.systemctl, "restart", args.service], check=False,
        text=True, capture_output=True)
    if result.returncode != 0:
        print(f"[remediator] restart failed: {result.stderr.strip()}",
              file=sys.stderr)
        return 1
    print(f"[remediator] restarted {args.service} once for incident {incident_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
