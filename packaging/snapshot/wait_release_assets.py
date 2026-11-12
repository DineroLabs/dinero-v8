#!/usr/bin/env python3
"""Wait for the signed full-v6 inputs before a tag-triggered Windows build.

Creating a draft GitHub release can also create its tag. The tag push starts
this workflow while the operator is still uploading the snapshot assets.
Bound that race, then let the existing download and byte/signature checks run.
"""

import argparse
import json
import math
import subprocess
import sys
import time


PRIMARY = "dinero-assumeutxo-150521-v6-full.dat"
REQUIRED = (
    PRIMARY,
    PRIMARY + ".manifest.json",
    PRIMARY + ".publisher.manifest.json",
    PRIMARY + ".publisher.manifest.sig",
)


def ready_names(tag: str, repo: str, query_timeout: float) -> set[str]:
    result = subprocess.run(
        ["gh", "release", "view", tag, "--repo", repo, "--json", "assets"],
        capture_output=True, text=True, check=False, timeout=query_timeout)
    if result.returncode:
        raise ValueError(result.stderr.strip() or "release is not visible yet")
    document = json.loads(result.stdout)
    assets = document.get("assets")
    if not isinstance(assets, list):
        raise ValueError("release assets response is missing its asset list")
    return {
        asset["name"] for asset in assets
        if isinstance(asset, dict) and isinstance(asset.get("name"), str)
        and asset.get("state") == "uploaded"
        and type(asset.get("size")) is int and asset["size"] > 0
    }


def wait(tag: str, repo: str, timeout_seconds: float, poll_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    last_report = None
    attempts = 0
    while True:
        attempts += 1
        remaining = max(0.05, min(30.0, deadline - time.monotonic()))
        try:
            present = ready_names(tag, repo, remaining)
            missing = tuple(name for name in REQUIRED if name not in present)
            detail = "missing: " + ", ".join(missing)
        except (ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as error:
            missing = REQUIRED
            detail = "release lookup pending: " + str(error)[:200]
        if not missing:
            print(f"four full-v6 release assets ready on {tag} after {attempts} checks", flush=True)
            return True
        if detail != last_report:
            print(detail, flush=True)
            last_report = detail
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            print(f"Timed out waiting for {tag} full-v6 release assets; {detail}", file=sys.stderr)
            return False
        time.sleep(min(poll_seconds, remaining))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--poll-seconds", type=float, default=10.0)
    args = parser.parse_args()
    if (not math.isfinite(args.timeout_seconds) or args.timeout_seconds < 0 or
            not math.isfinite(args.poll_seconds) or args.poll_seconds <= 0):
        parser.error("timeout must be nonnegative and poll interval must be positive")
    try:
        return 0 if wait(args.tag, args.repo, args.timeout_seconds, args.poll_seconds) else 1
    except FileNotFoundError as error:
        parser.exit(1, f"GitHub CLI is unavailable: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
