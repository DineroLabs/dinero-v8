#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import re
import sys
import time
import urllib.request
from pathlib import Path


BLOCK_FILE_RE = re.compile(r"^block(\d+)\.hex$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay archived blockNNN.hex files through submitblock on a zero-peer local node."
    )
    parser.add_argument("--archive-dir", required=True, help="Directory containing blockNNN.hex files")
    parser.add_argument("--rpc-url", help="RPC endpoint, for example http://127.0.0.1:20998/")
    parser.add_argument("--rpc-user", help="RPC username")
    parser.add_argument("--rpc-password", help="RPC password")
    parser.add_argument("--datadir", help="Datadir containing .cookie for local RPC auth")
    parser.add_argument("--rpc-port", type=int, default=20998, help="RPC port when using --datadir auth")
    parser.add_argument("--start-height", type=int, help="First height to replay")
    parser.add_argument("--end-height", type=int, help="Last height to replay")
    parser.add_argument("--progress-every", type=int, default=100, help="Print progress every N accepted blocks")
    parser.add_argument(
        "--allow-holes",
        action="store_true",
        help="Allow missing blockNNN.hex heights in the selected range instead of failing closed",
    )
    parser.add_argument(
        "--require-zero-peers",
        action="store_true",
        help="Fail if getconnectioncount is non-zero during replay",
    )
    parser.add_argument("--expected-start-height", type=int, help="Expected chain height before replay starts")
    parser.add_argument("--expected-final-height", type=int, help="Expected chain height after replay completes")
    parser.add_argument("--expected-final-hash", help="Expected best block hash after replay completes")
    parser.add_argument(
        "--expected-utreexo-commitment",
        help="Expected blockchain.getutreexocommitment.commitment after replay completes",
    )
    parser.add_argument(
        "--json-report",
        help="Optional path for a JSON replay report",
    )
    return parser.parse_args()


def parse_height(path: Path) -> int | None:
    match = BLOCK_FILE_RE.match(path.name)
    if not match:
        return None
    return int(match.group(1))


def cookie_credentials(datadir: Path) -> tuple[str, str]:
    candidates = [datadir / ".cookie", datadir / "regtest" / ".cookie", datadir / "testnet" / ".cookie"]
    for candidate in candidates:
        if candidate.exists():
            user, password = candidate.read_text().strip().split(":", 1)
            return user, password
    raise RuntimeError(f"No .cookie file found under {datadir}")


class RpcClient:
    def __init__(self, url: str, user: str, password: str) -> None:
        self._url = url
        auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self._headers = {
            "Content-Type": "application/json",
            "Authorization": "Basic " + auth,
        }

    def call(self, method: str, params: list, timeout: float = 60.0) -> object:
        body = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": "codex",
                "method": method,
                "params": params,
            }
        ).encode()
        request = urllib.request.Request(self._url, data=body, headers=self._headers)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.load(response)
        if payload.get("error"):
            raise RuntimeError(str(payload["error"]))
        return payload.get("result")


def build_client(args: argparse.Namespace) -> RpcClient:
    if args.rpc_url and args.rpc_user and args.rpc_password:
        return RpcClient(args.rpc_url, args.rpc_user, args.rpc_password)
    if args.datadir:
        user, password = cookie_credentials(Path(args.datadir))
        return RpcClient(f"http://127.0.0.1:{args.rpc_port}/", user, password)
    raise RuntimeError("Provide either --rpc-url/--rpc-user/--rpc-password or --datadir")


def block_metadata(path: Path) -> dict:
    raw = bytes.fromhex("".join(path.read_text().split()))
    if len(raw) < 128:
        raise RuntimeError(f"{path} is shorter than a 128-byte block header")
    header = raw[:128]
    block_hash = hashlib.sha256(hashlib.sha256(header).digest()).digest().hex()
    prev_hash = header[4:36][::-1].hex()
    return {
        "path": str(path),
        "raw_hex": raw.hex(),
        "hash": block_hash,
        "prev_hash": prev_hash,
    }


def collect_archive(archive_dir: Path, start_height: int | None, end_height: int | None) -> tuple[list[int], dict[int, Path], list[int]]:
    exports: dict[int, Path] = {}
    for path in archive_dir.iterdir():
        if not path.is_file():
            continue
        height = parse_height(path)
        if height is None or path.stat().st_size <= 0:
            continue
        exports[height] = path
    if not exports:
        raise RuntimeError(f"No blockNNN.hex files found under {archive_dir}")

    selected_start = min(exports) if start_height is None else start_height
    selected_end = max(exports) if end_height is None else end_height
    if selected_end < selected_start:
        raise RuntimeError(f"end height {selected_end} is below start height {selected_start}")

    heights = [height for height in sorted(exports) if selected_start <= height <= selected_end]
    missing = [height for height in range(selected_start, selected_end + 1) if height not in exports]
    return heights, exports, missing


def assert_zero_peers(client: RpcClient) -> int:
    peers = client.call("getconnectioncount", [])
    if not isinstance(peers, int):
        raise RuntimeError(f"Unexpected getconnectioncount result: {peers!r}")
    if peers != 0:
        raise RuntimeError(f"Expected zero peers during offline replay, got {peers}")
    return peers


def maybe_get_block(client: RpcClient, block_hash: str) -> dict | None:
    try:
        result = client.call("getblock", [block_hash, 1], timeout=30)
    except Exception:
        return None
    if isinstance(result, dict):
        return result
    return None


def fetch_chain_height(client: RpcClient) -> int:
    info = client.call("getblockchaininfo", [])
    if not isinstance(info, dict) or "blocks" not in info:
        raise RuntimeError(f"Unexpected getblockchaininfo result: {info!r}")
    return int(info["blocks"])


def fetch_chain_state(client: RpcClient) -> dict:
    info = client.call("getblockchaininfo", [])
    if not isinstance(info, dict):
        raise RuntimeError(f"Unexpected getblockchaininfo result: {info!r}")
    commitment = client.call("blockchain.getutreexocommitment", [])
    if not isinstance(commitment, dict):
        raise RuntimeError(f"Unexpected getutreexocommitment result: {commitment!r}")
    return {
        "blocks": int(info["blocks"]),
        "bestblockhash": info.get("bestblockhash"),
        "headers": int(info.get("headers", info["blocks"])),
        "utreexo_commitment": commitment.get("commitment"),
        "utreexo_num_leaves": commitment.get("num_leaves"),
        "utreexo_num_roots": commitment.get("num_roots"),
    }


def main() -> int:
    args = parse_args()
    client = build_client(args)
    archive_dir = Path(args.archive_dir)

    heights, exports, missing = collect_archive(archive_dir, args.start_height, args.end_height)
    if not heights:
        raise SystemExit("No exported block files matched the requested height range")
    if missing and not args.allow_holes:
        raise SystemExit(
            "Refusing replay with holes inside selected range. Missing heights: "
            f"{missing[:50]}"
        )

    if args.expected_start_height is not None:
        current_height = fetch_chain_height(client)
        if current_height != args.expected_start_height:
            raise SystemExit(
                f"Unexpected starting height {current_height}; expected {args.expected_start_height}"
            )

    if args.require_zero_peers:
        assert_zero_peers(client)

    last_hash = None
    accepted = 0
    skipped = 0
    started = time.time()

    for prev_height, next_height in zip(heights, heights[1:]):
        if next_height == prev_height + 1:
            prev_meta = block_metadata(exports[prev_height])
            next_meta = block_metadata(exports[next_height])
            if next_meta["prev_hash"] != prev_meta["hash"]:
                raise SystemExit(
                    "Archive parent linkage mismatch: "
                    f"height {next_height} prev={next_meta['prev_hash']} "
                    f"expected {prev_meta['hash']}"
                )

    for height in heights:
        meta = block_metadata(exports[height])
        existing = maybe_get_block(client, meta["hash"])
        if existing is not None:
            existing_height = int(existing.get("height", -1))
            if existing_height != height:
                raise SystemExit(
                    f"Block {meta['hash']} already known at height {existing_height}, expected {height}"
                )
            skipped += 1
            last_hash = meta["hash"]
            continue

        submit_result = client.call("submitblock", [meta["raw_hex"]], timeout=120)
        if isinstance(submit_result, dict) and submit_result.get("error"):
            raise SystemExit(f"submitblock rejected height {height}: {submit_result}")

        accepted_block = maybe_get_block(client, meta["hash"])
        if accepted_block is None:
            raise SystemExit(
                f"submitblock returned success for height {height}, but block {meta['hash']} is not retrievable"
            )

        accepted_height = int(accepted_block.get("height", -1))
        if accepted_height != height:
            raise SystemExit(
                f"Accepted block height mismatch for {meta['hash']}: got {accepted_height}, expected {height}"
            )

        accepted += 1
        last_hash = meta["hash"]

        if args.require_zero_peers and (accepted <= 3 or accepted % args.progress_every == 0):
            assert_zero_peers(client)

        if accepted <= 5 or accepted % args.progress_every == 0:
            elapsed = max(time.time() - started, 0.001)
            rate = accepted / elapsed
            chain_height = fetch_chain_height(client)
            print(
                f"accepted height={height} hash={meta['hash']} chain_height={chain_height} "
                f"accepted={accepted} skipped={skipped} rate={rate:.2f}/s",
                flush=True,
            )

    final_state = fetch_chain_state(client)
    if args.require_zero_peers:
        final_state["connections"] = assert_zero_peers(client)

    if args.expected_final_height is not None and final_state["blocks"] != args.expected_final_height:
        raise SystemExit(
            f"Unexpected final height {final_state['blocks']}; expected {args.expected_final_height}"
        )
    if args.expected_final_hash is not None and final_state["bestblockhash"] != args.expected_final_hash:
        raise SystemExit(
            f"Unexpected final best hash {final_state['bestblockhash']}; expected {args.expected_final_hash}"
        )
    if (
        args.expected_utreexo_commitment is not None
        and final_state["utreexo_commitment"] != args.expected_utreexo_commitment
    ):
        raise SystemExit(
            "Unexpected Utreexo commitment "
            f"{final_state['utreexo_commitment']}; expected {args.expected_utreexo_commitment}"
        )

    report = {
        "archive_dir": str(archive_dir),
        "start_height": heights[0],
        "end_height": heights[-1],
        "accepted": accepted,
        "skipped": skipped,
        "last_hash": last_hash,
        "missing_count": len(missing),
        "first_missing_heights": missing[:100],
        "final_state": final_state,
    }
    if args.json_report:
        Path(args.json_report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    print(json.dumps(report, indent=2, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
