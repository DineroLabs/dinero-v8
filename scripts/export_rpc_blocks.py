#!/usr/bin/env python3

import argparse
import base64
import json
import os
import re
import sys
import time
import urllib.request
from pathlib import Path
from typing import Iterable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export raw block bodies from a JSON-RPC node.")
    parser.add_argument("--rpc-url", help="RPC endpoint, for example http://127.0.0.1:20998/")
    parser.add_argument("--rpc-user", help="RPC username")
    parser.add_argument("--rpc-password", help="RPC password")
    parser.add_argument("--start-height", type=int, help="First height to export or audit")
    parser.add_argument("--end-height", type=int, help="Last height to export or audit")
    parser.add_argument("--out-dir", required=True, help="Directory for blockNNN.hex files")
    parser.add_argument("--max-retries", type=int, default=10, help="Retries per block on RPC failures")
    parser.add_argument("--retry-delay", type=float, default=5.0, help="Seconds to wait between retries")
    parser.add_argument("--progress-every", type=int, default=100, help="Print progress every N exported blocks")
    parser.add_argument(
        "--fill-holes",
        action="store_true",
        help="Export only missing heights inside the requested range instead of walking every height",
    )
    parser.add_argument(
        "--resume-to-tip",
        action="store_true",
        help="Resolve the end height from the remote node tip",
    )
    parser.add_argument(
        "--audit-only",
        action="store_true",
        help="Inspect the local archive and print/write a manifest without exporting blocks",
    )
    parser.add_argument(
        "--write-manifest",
        help="Optional path for a JSON archive manifest",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Log a failed height and continue with later heights instead of exiting",
    )
    return parser.parse_args()


class RpcClient:
    def __init__(self, url: str, user: str, password: str) -> None:
        self._url = url
        auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self._headers = {
            "Content-Type": "text/plain",
            "Authorization": "Basic " + auth,
        }

    def call(self, method: str, params: list, timeout: float) -> object:
        body = json.dumps({
            "jsonrpc": "1.0",
            "id": "codex",
            "method": method,
            "params": params,
        }).encode()
        request = urllib.request.Request(self._url, data=body, headers=self._headers)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.load(response)
        if payload.get("error"):
            raise RuntimeError(str(payload["error"]))
        return payload["result"]


BLOCK_FILE_RE = re.compile(r"^block(\d+)\.hex$")


def parse_height_from_path(path: Path) -> int | None:
    match = BLOCK_FILE_RE.match(path.name)
    if not match:
        return None
    return int(match.group(1))


def collect_archive_heights(out_dir: Path) -> list[int]:
    heights: list[int] = []
    for path in out_dir.iterdir():
        if not path.is_file():
            continue
        height = parse_height_from_path(path)
        if height is None:
            continue
        if path.stat().st_size <= 0:
            continue
        heights.append(height)
    heights.sort()
    return heights


def summarize_heights(heights: list[int], start_height: int | None, end_height: int | None) -> dict:
    summary = {
        "file_count": len(heights),
        "min_height": heights[0] if heights else None,
        "max_height": heights[-1] if heights else None,
        "first_missing_heights": [],
        "missing_count": 0,
    }
    if start_height is None or end_height is None or end_height < start_height:
        return summary

    existing = set(heights)
    missing: list[int] = []
    for height in range(start_height, end_height + 1):
        if height not in existing:
            missing.append(height)
    summary["range_start"] = start_height
    summary["range_end"] = end_height
    summary["missing_count"] = len(missing)
    summary["first_missing_heights"] = missing[:100]
    return summary


def print_summary(summary: dict) -> None:
    print(
        "archive files={file_count} min_height={min_height} max_height={max_height}".format(
            file_count=summary["file_count"],
            min_height=summary["min_height"],
            max_height=summary["max_height"],
        ),
        flush=True,
    )
    if "range_start" in summary:
        print(
            "range start={range_start} end={range_end} missing={missing_count} first_missing={first_missing}".format(
                range_start=summary["range_start"],
                range_end=summary["range_end"],
                missing_count=summary["missing_count"],
                first_missing=summary["first_missing_heights"],
            ),
            flush=True,
        )


def write_manifest(path: Path, summary: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")


def resolve_end_height(args: argparse.Namespace, client: RpcClient | None) -> int | None:
    if args.resume_to_tip:
        if client is None:
            raise RuntimeError("--resume-to-tip requires RPC access")
        remote_tip = client.call("getblockcount", [], timeout=30)
        if not isinstance(remote_tip, int):
            raise RuntimeError(f"Unexpected getblockcount result: {remote_tip!r}")
        return remote_tip
    return args.end_height


def build_export_heights(
    existing_heights: list[int],
    start_height: int,
    end_height: int,
    fill_holes: bool,
) -> Iterable[int]:
    if not fill_holes:
        return range(start_height, end_height + 1)

    existing = set(existing_heights)
    return [height for height in range(start_height, end_height + 1) if height not in existing]


def export_block(client: RpcClient, height: int, out_dir: Path, retries: int, retry_delay: float) -> tuple[str, int]:
    target = out_dir / f"block{height}.hex"
    if target.exists() and target.stat().st_size > 0:
        return "", 0

    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            block_hash = client.call("getblockhash", [height], timeout=30)
            block_hex = client.call("getblock", [block_hash, 0], timeout=90)
            if not isinstance(block_hash, str) or not isinstance(block_hex, str):
                raise RuntimeError(f"Unexpected RPC result types for height {height}")
            tmp = target.with_suffix(".hex.tmp")
            tmp.write_text(block_hex)
            os.replace(tmp, target)
            return block_hash, len(block_hex)
        except Exception as exc:  # pragma: no cover - best effort ops tool
            last_error = exc
            print(f"retry height={height} attempt={attempt}/{retries} error={exc}", flush=True)
            time.sleep(retry_delay)

    raise RuntimeError(f"failed height={height} after {retries} attempts: {last_error}")


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    need_rpc = args.resume_to_tip or not args.audit_only
    client = None
    if need_rpc:
        if not args.rpc_url or not args.rpc_user or not args.rpc_password:
            raise SystemExit(
                "--rpc-url, --rpc-user, and --rpc-password are required unless running a purely local audit"
            )
        client = RpcClient(args.rpc_url, args.rpc_user, args.rpc_password)
    end_height = resolve_end_height(args, client if (args.resume_to_tip or not args.audit_only) else None)

    existing_heights = collect_archive_heights(out_dir)
    print(f"resuming with {len(existing_heights)} existing files in {out_dir}", flush=True)

    summary = summarize_heights(existing_heights, args.start_height, end_height)
    print_summary(summary)
    if args.write_manifest:
        write_manifest(Path(args.write_manifest), summary)

    if args.audit_only:
        return 0

    if args.start_height is None:
        raise SystemExit("--start-height is required unless --audit-only is used")
    if end_height is None:
        raise SystemExit("--end-height or --resume-to-tip is required for export mode")
    if end_height < args.start_height:
        raise SystemExit(f"end height {end_height} is below start height {args.start_height}")

    exported = 0
    started = time.time()

    export_heights = list(build_export_heights(existing_heights, args.start_height, end_height, args.fill_holes))
    print(f"target heights to fetch: {len(export_heights)}", flush=True)

    for height in export_heights:
        try:
            block_hash, hex_len = export_block(
                client=client,
                height=height,
                out_dir=out_dir,
                retries=args.max_retries,
                retry_delay=args.retry_delay,
            )
        except Exception as exc:  # pragma: no cover - ops script
            print(f"failed height={height} error={exc}", flush=True)
            if args.continue_on_error:
                continue
            raise
        if not block_hash:
            continue
        exported += 1
        if exported <= 5 or exported % args.progress_every == 0:
            elapsed = max(time.time() - started, 0.001)
            rate = exported / elapsed
            print(
                f"fetched height={height} hash={block_hash} hex_len={hex_len} "
                f"files={exported} rate={rate:.2f}/s",
                flush=True,
            )

    final_summary = summarize_heights(collect_archive_heights(out_dir), args.start_height, end_height)
    print_summary(final_summary)
    if args.write_manifest:
        write_manifest(Path(args.write_manifest), final_summary)
    print(f"complete heights {args.start_height}-{end_height}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
