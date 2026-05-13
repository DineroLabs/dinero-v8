#!/usr/bin/env python3
"""
CI Invariant Test: Utreexo Commitment Determinism
==================================================

This test verifies a critical invariant:
  getblocktemplate called twice → utreexo.commitment MUST be identical

This single assertion prevents:
- Thread races in Utreexo accumulator
- Accidental nondeterminism in block template generation
- "Works on my machine" regressions

Usage:
    python3 test_utreexo_determinism.py [--rpc-url URL] [--address ADDR]

Exit codes:
    0 = PASS (commitment is deterministic)
    1 = FAIL (commitment differs or test error)
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Default configuration
DEFAULT_RPC_PORT = 20996
DEFAULT_ADDRESS = "rdin1p86vgfwjzv8wdk8wyfvyfafhkk02vkjutd4sqygqurqyfkktthhxqxpc8nz"


def rpc_call(method: str, params: list, rpc_url: str, cookie_path: str = None) -> dict:
    """Make JSON-RPC call to daemon"""

    # Read cookie if available
    auth = None
    if cookie_path and os.path.exists(cookie_path):
        with open(cookie_path, 'r') as f:
            cookie = f.read().strip()
            if ':' in cookie:
                auth = cookie

    payload = {
        "jsonrpc": "2.0",
        "id": "utreexo-determinism-test",
        "method": method,
        "params": params
    }

    curl_cmd = ["curl", "-s", "-X", "POST", "-H", "Content-Type: application/json"]

    if auth:
        curl_cmd.extend(["-u", auth])

    curl_cmd.extend(["-d", json.dumps(payload), rpc_url])

    try:
        result = subprocess.run(curl_cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            return {"error": {"message": f"curl failed: {result.stderr}"}}

        return json.loads(result.stdout)
    except subprocess.TimeoutExpired:
        return {"error": {"message": "RPC timeout"}}
    except json.JSONDecodeError as e:
        return {"error": {"message": f"JSON decode error: {e}"}}
    except Exception as e:
        return {"error": {"message": str(e)}}


def get_utreexo_commitment(rpc_url: str, address: str, cookie_path: str = None) -> tuple:
    """
    Call getblocktemplate and extract utreexo commitment.
    Returns (commitment, height, error_message)
    """
    params = [{"address": address}]
    response = rpc_call("getblocktemplate", params, rpc_url, cookie_path)

    if "error" in response and response["error"]:
        return None, None, response["error"].get("message", str(response["error"]))

    result = response.get("result", {})

    # Check for RPC-level error in result
    if isinstance(result, dict) and "error" in result:
        return None, None, result["error"]

    # Extract commitment from structured utreexo object (preferred)
    commitment = None
    if "utreexo" in result and isinstance(result["utreexo"], dict):
        commitment = result["utreexo"].get("commitment")

    # Fallback to legacy field
    if not commitment:
        commitment = result.get("utreexocommitment")

    height = result.get("height")

    if not commitment:
        return None, height, "No utreexo commitment in response"

    return commitment, height, None


def test_utreexo_determinism(rpc_url: str, address: str, cookie_path: str = None,
                              iterations: int = 5, delay_ms: int = 0) -> bool:
    """
    Test that getblocktemplate returns deterministic utreexo commitment.

    Args:
        rpc_url: Daemon RPC URL
        address: Mining address for getblocktemplate
        cookie_path: Path to .cookie file for auth
        iterations: Number of times to call getblocktemplate
        delay_ms: Delay between calls (to test timing sensitivity)

    Returns:
        True if all commitments match, False otherwise
    """
    print(f"{'='*60}")
    print("CI INVARIANT: Utreexo Commitment Determinism")
    print(f"{'='*60}")
    print(f"RPC URL: {rpc_url}")
    print(f"Address: {address[:20]}...")
    print(f"Iterations: {iterations}")
    print(f"Delay: {delay_ms}ms")
    print()

    commitments = []
    heights = []

    for i in range(iterations):
        if delay_ms > 0 and i > 0:
            time.sleep(delay_ms / 1000.0)

        commitment, height, error = get_utreexo_commitment(rpc_url, address, cookie_path)

        if error:
            print(f"[{i+1}/{iterations}] ERROR: {error}")
            return False

        commitments.append(commitment)
        heights.append(height)

        # Show first 16 chars for readability
        short_commit = commitment[:16] if commitment else "None"
        print(f"[{i+1}/{iterations}] Height={height} Commitment={short_commit}...")

    print()

    # Verify all commitments are identical
    unique_commitments = set(commitments)
    unique_heights = set(heights)

    if len(unique_heights) > 1:
        print(f"WARNING: Height changed during test ({unique_heights})")
        print("         This may cause commitment to change legitimately.")
        print("         Re-running test...")
        return test_utreexo_determinism(rpc_url, address, cookie_path, iterations, delay_ms)

    if len(unique_commitments) == 1:
        print(f"PASS: All {iterations} commitments identical")
        print(f"      Commitment: {commitments[0]}")
        print(f"      Height: {heights[0]}")
        return True
    else:
        print(f"FAIL: Commitments differ!")
        print(f"      Found {len(unique_commitments)} unique values:")
        for i, c in enumerate(commitments):
            print(f"        [{i+1}] {c}")
        return False


def test_parallel_determinism(rpc_url: str, address: str, cookie_path: str = None) -> bool:
    """
    Test determinism under parallel requests (stress test for thread safety).
    Spawns multiple concurrent requests and verifies all return same commitment.
    """
    import concurrent.futures

    print(f"\n{'='*60}")
    print("STRESS TEST: Parallel Request Determinism")
    print(f"{'='*60}")

    num_parallel = 10

    def get_commitment(_):
        return get_utreexo_commitment(rpc_url, address, cookie_path)

    with concurrent.futures.ThreadPoolExecutor(max_workers=num_parallel) as executor:
        futures = [executor.submit(get_commitment, i) for i in range(num_parallel)]
        results = [f.result() for f in concurrent.futures.as_completed(futures)]

    commitments = []
    heights = []
    errors = []

    for commitment, height, error in results:
        if error:
            errors.append(error)
        else:
            commitments.append(commitment)
            heights.append(height)

    if errors:
        print(f"FAIL: {len(errors)} requests failed")
        for e in errors[:3]:
            print(f"      {e}")
        return False

    unique_commitments = set(commitments)
    unique_heights = set(heights)

    if len(unique_heights) > 1:
        print(f"WARNING: Height changed during parallel test, re-running...")
        return test_parallel_determinism(rpc_url, address, cookie_path)

    if len(unique_commitments) == 1:
        print(f"PASS: All {num_parallel} parallel requests returned identical commitment")
        return True
    else:
        print(f"FAIL: Parallel requests returned different commitments!")
        print(f"      Found {len(unique_commitments)} unique values")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Test Utreexo commitment determinism invariant"
    )
    parser.add_argument(
        "--rpc-url",
        default=f"http://127.0.0.1:{DEFAULT_RPC_PORT}",
        help=f"Daemon RPC URL (default: http://127.0.0.1:{DEFAULT_RPC_PORT})"
    )
    parser.add_argument(
        "--address",
        default=DEFAULT_ADDRESS,
        help="Mining address for getblocktemplate"
    )
    parser.add_argument(
        "--cookie",
        default=os.path.expanduser("~/.dinero/.cookie"),
        help="Path to .cookie file for authentication"
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=5,
        help="Number of sequential calls (default: 5)"
    )
    parser.add_argument(
        "--delay",
        type=int,
        default=0,
        help="Delay between calls in milliseconds (default: 0)"
    )
    parser.add_argument(
        "--parallel",
        action="store_true",
        help="Also run parallel stress test"
    )
    parser.add_argument(
        "--ci",
        action="store_true",
        help="CI mode: exit 1 on any failure"
    )

    args = parser.parse_args()

    # Run sequential determinism test
    seq_pass = test_utreexo_determinism(
        args.rpc_url,
        args.address,
        args.cookie,
        args.iterations,
        args.delay
    )

    # Run parallel test if requested
    par_pass = True
    if args.parallel:
        par_pass = test_parallel_determinism(
            args.rpc_url,
            args.address,
            args.cookie
        )

    print(f"\n{'='*60}")
    if seq_pass and par_pass:
        print("RESULT: ALL TESTS PASSED")
        print("        Utreexo commitment is deterministic")
        print(f"{'='*60}")
        return 0
    else:
        print("RESULT: TESTS FAILED")
        print("        Utreexo commitment is NOT deterministic!")
        print("        This is a consensus-critical bug.")
        print(f"{'='*60}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
