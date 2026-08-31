#!/usr/bin/env python3
"""Read-only covenant activation safety checker.

Checks the source-pinned activation/checksum and, when --node is supplied,
compares live fleet height, tip, peer count, and consensus checksum. It never
submits transactions, changes configuration, or restarts a node.
"""

from __future__ import annotations

import argparse
import base64
import json
import pathlib
import re
import sys
import urllib.request


ACTIVATION_HEIGHT = 100_000
EXPECTED_CHECKSUM = "68e0a99766e8ab1224ee040ec715bbbd0a544a59d4b3a96025dd35f77f4e960a"


def rpc(url: str, cookie: str, method: str) -> object:
    credentials = pathlib.Path(cookie).read_text(encoding="utf-8").strip()
    token = base64.b64encode(credentials.encode()).decode()
    request = urllib.request.Request(
        url,
        data=json.dumps(
            {"jsonrpc": "2.0", "id": "covenant-safety", "method": method, "params": []}
        ).encode(),
        headers={"Authorization": f"Basic {token}", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = json.load(response)
    if payload.get("error") is not None:
        raise RuntimeError(f"{method}: {payload['error']}")
    return payload["result"]


def parse_node(value: str) -> tuple[str, str, str]:
    parts = value.split(",", 2)
    if len(parts) != 3 or not all(parts):
        raise argparse.ArgumentTypeError("node must be NAME,URL,COOKIE_PATH")
    return parts[0], parts[1], parts[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-root",
        default=str(pathlib.Path(__file__).resolve().parents[1]),
    )
    parser.add_argument("--node", action="append", type=parse_node, default=[])
    parser.add_argument(
        "--require-before-activation",
        action="store_true",
        help="fail if any node has already reached block 100000",
    )
    args = parser.parse_args()

    source = pathlib.Path(args.source_root)
    chainparams = (source / "src/consensus/chainparams_impl.cpp").read_text()
    activation_match = re.search(
        r"MAINNET_COVENANT_PROFILE_V1_HEIGHT\s*=\s*([0-9']+)", chainparams
    )
    source_height = (
        int(activation_match.group(1).replace("'", "")) if activation_match else None
    )
    checks: list[dict[str, object]] = [
        {
            "name": "source_activation_height",
            "passed": source_height == ACTIVATION_HEIGHT,
            "actual": source_height,
            "expected": ACTIVATION_HEIGHT,
        }
    ]

    nodes: list[dict[str, object]] = []
    for name, url, cookie in args.node:
        height = int(rpc(url, cookie, "getblockcount"))
        tip = str(rpc(url, cookie, "getbestblockhash"))
        peers = int(rpc(url, cookie, "getconnectioncount"))
        consensus = rpc(url, cookie, "getconsensusinfo")
        checksum = str(consensus.get("consensus_checksum", ""))
        nodes.append(
            {
                "name": name,
                "height": height,
                "tip": tip,
                "peers": peers,
                "consensus_checksum": checksum,
                "blocks_remaining": ACTIVATION_HEIGHT - height,
            }
        )
        checks.extend(
            [
                {
                    "name": f"{name}_checksum",
                    "passed": checksum == EXPECTED_CHECKSUM,
                    "actual": checksum,
                    "expected": EXPECTED_CHECKSUM,
                },
                {
                    "name": f"{name}_has_peers",
                    "passed": peers > 0,
                    "actual": peers,
                    "expected": ">0",
                },
                {
                    "name": f"{name}_before_activation",
                    "passed": not args.require_before_activation or height < ACTIVATION_HEIGHT,
                    "actual": height,
                    "expected": f"<{ACTIVATION_HEIGHT}",
                },
            ]
        )

    if nodes:
        heights = {node["height"] for node in nodes}
        tips = {node["tip"] for node in nodes}
        checks.extend(
            [
                {
                    "name": "fleet_same_height",
                    "passed": len(heights) == 1,
                    "actual": sorted(heights),
                    "expected": "one shared height",
                },
                {
                    "name": "fleet_same_tip",
                    "passed": len(tips) == 1,
                    "actual": sorted(tips),
                    "expected": "one shared hash",
                },
            ]
        )

    passed = all(bool(check["passed"]) for check in checks)
    print(
        json.dumps(
            {
                "schema": "din.covenant.activation-safety.v1",
                "passed": passed,
                "activation_height": ACTIVATION_HEIGHT,
                "expected_checksum": EXPECTED_CHECKSUM,
                "nodes": nodes,
                "checks": checks,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
