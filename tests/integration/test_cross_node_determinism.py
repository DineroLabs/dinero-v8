#!/usr/bin/env python3
"""
CI Invariant Test: Cross-Node Utreexo Determinism
==================================================

This test verifies a critical invariant:
  Two independent nodes with identical chainstate MUST produce
  identical Utreexo commitments in getblocktemplate.

This proves:
- Accumulator computation is deterministic across processes
- No hidden state or random seeds affect commitment
- Nodes can be safely replaced/restarted without divergence
- Pool infrastructure with multiple daemons is safe

Methodology:
1. Start two independent daemon instances (node_a, node_b)
2. Connect them as peers (P2P sync)
3. Mine blocks to height >= 2 (Utreexo activation)
4. Wait for both nodes to sync to same tip
5. Call getblocktemplate on both nodes
6. Assert utreexo.commitment is IDENTICAL

Usage:
    python3 test_cross_node_determinism.py [--dinerod PATH] [--datadir PATH]

Exit codes:
    0 = PASS (cross-node commitment matches)
    1 = FAIL (commitment differs or test error)
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

# Test configuration
DEFAULT_ADDRESS = "rdin1p86vgfwjzv8wdk8wyfvyfafhkk02vkjutd4sqygqurqyfkktthhxqxpc8nz"

class DineroNode:
    """Wrapper for a single dinerod instance"""

    def __init__(self, name: str, datadir: Path, rpc_port: int, p2p_port: int, dinerod_path: str):
        self.name = name
        self.datadir = datadir
        self.rpc_port = rpc_port
        self.p2p_port = p2p_port
        self.dinerod = dinerod_path
        self.process: Optional[subprocess.Popen] = None
        self.cookie: Optional[str] = None

    def setup(self, connect_to: Optional[int] = None):
        """Create datadir and config"""
        if self.datadir.exists():
            shutil.rmtree(self.datadir)
        self.datadir.mkdir(parents=True, exist_ok=True)

        config = self.datadir / "dinero.conf"
        config_content = f"""regtest=1
server=1
daemon=0
rpcport={self.rpc_port}
port={self.p2p_port}
fallbackfee=0.00001
txindex=1
debug=1
"""
        if connect_to:
            config_content += f"connect=127.0.0.1:{connect_to}\n"

        config.write_text(config_content)
        print(f"[{self.name}] Config created at {config}")

    def start(self) -> bool:
        """Start the daemon"""
        print(f"[{self.name}] Starting daemon on RPC:{self.rpc_port} P2P:{self.p2p_port}...")

        # Note: Must pass ports on command line - config file rpcport is ignored
        self.process = subprocess.Popen(
            [
                self.dinerod,
                f"--datadir={self.datadir}",
                "--regtest",
                f"--rpcport={self.rpc_port}",
                f"--port={self.p2p_port}",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        # Wait for RPC to be ready
        for i in range(30):
            time.sleep(1)
            self._read_cookie()
            if self.cookie:
                result = self.rpc("getinfo")
                if result and "blocks" in result:
                    print(f"[{self.name}] Ready at height {result.get('blocks', 0)}")
                    return True
            print(f"[{self.name}] Waiting... ({i+1}s)")

        print(f"[{self.name}] FAILED to start")
        return False

    def stop(self):
        """Stop the daemon"""
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
            print(f"[{self.name}] Stopped")

    def _read_cookie(self):
        """Read authentication cookie"""
        cookie_path = self.datadir / "regtest" / ".cookie"
        if not cookie_path.exists():
            cookie_path = self.datadir / ".cookie"

        if cookie_path.exists():
            self.cookie = cookie_path.read_text().strip()

    def rpc(self, method: str, params: list = None, timeout: int = 60) -> Optional[dict]:
        """Make RPC call"""
        if not self.cookie:
            self._read_cookie()

        if not self.cookie:
            return None

        payload = {
            "jsonrpc": "2.0",
            "id": f"{self.name}-{method}",
            "method": method,
            "params": params or []
        }

        try:
            result = subprocess.run(
                [
                    "curl", "-s", "-X", "POST",
                    "--max-time", str(timeout),
                    "-H", "Content-Type: application/json",
                    "-u", self.cookie,
                    "-d", json.dumps(payload),
                    f"http://127.0.0.1:{self.rpc_port}"
                ],
                capture_output=True,
                text=True,
                timeout=timeout + 5
            )

            if result.returncode != 0:
                print(f"[{self.name}] curl failed (rc={result.returncode})")
                print(f"[{self.name}] DEBUG: URL=http://127.0.0.1:{self.rpc_port}")
                print(f"[{self.name}] DEBUG: method={method}")
                print(f"[{self.name}] DEBUG: stderr={result.stderr[:200] if result.stderr else 'empty'}")
                return None

            if not result.stdout.strip():
                print(f"[{self.name}] Empty response from RPC")
                return None

            response = json.loads(result.stdout)
            if "error" in response and response["error"]:
                return {"error": response["error"]}
            return response.get("result", {})

        except json.JSONDecodeError as e:
            print(f"[{self.name}] JSON decode error: {e}, stdout: {result.stdout[:200]}")
            return None
        except Exception as e:
            print(f"[{self.name}] RPC exception: {e}")
            return {"error": str(e)}

    def get_height(self) -> int:
        """Get current block height"""
        result = self.rpc("getblockcount")
        if isinstance(result, int):
            return result
        return result.get("result", 0) if isinstance(result, dict) else 0

    def get_best_hash(self) -> str:
        """Get best block hash"""
        result = self.rpc("getbestblockhash")
        if isinstance(result, str):
            return result
        return ""

    def get_utreexo_commitment(self, address: str) -> Tuple[Optional[str], Optional[int], Optional[str]]:
        """Get Utreexo commitment from getblocktemplate"""
        result = self.rpc("getblocktemplate", [{"address": address}])

        if not result:
            return None, None, "RPC failed"

        if "error" in result:
            return None, None, str(result["error"])

        # Extract commitment
        commitment = None
        if "utreexo" in result and isinstance(result["utreexo"], dict):
            commitment = result["utreexo"].get("commitment")
        if not commitment:
            commitment = result.get("utreexocommitment")

        height = result.get("height")

        if not commitment:
            return None, height, "No utreexo commitment in response"

        return commitment, height, None

    def get_wallet_address(self) -> Optional[str]:
        """Get a new address from the wallet for mining"""
        result = self.rpc("wallet.getnewaddress", [], timeout=10)
        if result and isinstance(result, dict) and "address" in result:
            return result["address"]
        return None

    def mine_blocks(self, count: int, address: str = None) -> bool:
        """Mine blocks using the 'generate' RPC (designed for regtest)"""

        print(f"[{self.name}] Mining {count} blocks using generate RPC...")
        print(f"[{self.name}] DEBUG: RPC port={self.rpc_port}, cookie={self.cookie[:20] if self.cookie else 'None'}...")

        # Test if daemon is still responsive
        test_result = self.rpc("getinfo", [], timeout=5)
        if test_result:
            print(f"[{self.name}] DEBUG: Pre-mining getinfo OK, height={test_result.get('blocks', '?')}")
        else:
            print(f"[{self.name}] DEBUG: Pre-mining getinfo FAILED!")

        initial_height = self.get_height()

        # Use 'generate' RPC which is designed for regtest mining
        # This is a synchronous call that mines blocks and returns
        result = self.rpc("generate", [count], timeout=120)

        if result is None:
            print(f"[{self.name}] Generate RPC returned None")
            return False

        if isinstance(result, dict) and "error" in result:
            print(f"[{self.name}] Generate RPC error: {result.get('error')}")
            return False

        final_height = self.get_height()
        blocks_mined = final_height - initial_height

        if blocks_mined >= count:
            print(f"[{self.name}] Mined {blocks_mined} blocks (height: {initial_height} -> {final_height})")
            return True
        else:
            print(f"[{self.name}] Only mined {blocks_mined}/{count} blocks")
            return False


def wait_for_sync(node_a: DineroNode, node_b: DineroNode, timeout: int = 60) -> bool:
    """Wait for both nodes to sync to the same tip"""
    print("\nWaiting for nodes to sync...")

    start = time.time()
    while time.time() - start < timeout:
        height_a = node_a.get_height()
        height_b = node_b.get_height()
        hash_a = node_a.get_best_hash()
        hash_b = node_b.get_best_hash()

        print(f"  Node A: height={height_a} tip={hash_a[:16] if hash_a else 'N/A'}...")
        print(f"  Node B: height={height_b} tip={hash_b[:16] if hash_b else 'N/A'}...")

        if height_a == height_b and hash_a == hash_b and height_a > 0:
            print(f"Nodes synced at height {height_a}")
            return True

        time.sleep(2)

    print("TIMEOUT: Nodes failed to sync")
    return False


def test_cross_node_determinism(dinerod_path: str, base_datadir: Path, address: str) -> bool:
    """
    Test that two independent nodes produce identical Utreexo commitments.
    """
    print("=" * 70)
    print("CI INVARIANT: Cross-Node Utreexo Determinism")
    print("=" * 70)

    # Create two nodes
    node_a = DineroNode(
        name="node_a",
        datadir=base_datadir / "node_a",
        rpc_port=18432,
        p2p_port=18433,
        dinerod_path=dinerod_path
    )

    node_b = DineroNode(
        name="node_b",
        datadir=base_datadir / "node_b",
        rpc_port=18532,
        p2p_port=18533,
        dinerod_path=dinerod_path
    )

    try:
        # Setup nodes (node_b connects to node_a)
        print("\n[1/6] Setting up nodes...")
        node_a.setup()
        node_b.setup(connect_to=node_a.p2p_port)

        # Start nodes
        print("\n[2/6] Starting nodes...")
        if not node_a.start():
            print("FAIL: Node A failed to start")
            return False

        if not node_b.start():
            print("FAIL: Node B failed to start")
            return False

        # Mine blocks on node_a to height >= 3 (Utreexo active from height 2)
        print("\n[3/6] Mining blocks on Node A...")
        if not node_a.mine_blocks(5):  # Uses wallet-generated address
            print("FAIL: Mining failed")
            return False

        # Wait for sync
        print("\n[4/6] Waiting for P2P sync...")
        if not wait_for_sync(node_a, node_b):
            print("FAIL: Nodes failed to sync")
            return False

        # Get commitments from both nodes
        print("\n[5/6] Getting Utreexo commitments...")

        commit_a, height_a, err_a = node_a.get_utreexo_commitment(address)
        if err_a:
            print(f"FAIL: Node A error: {err_a}")
            return False

        commit_b, height_b, err_b = node_b.get_utreexo_commitment(address)
        if err_b:
            print(f"FAIL: Node B error: {err_b}")
            return False

        print(f"  Node A: height={height_a} commitment={commit_a[:24]}...")
        print(f"  Node B: height={height_b} commitment={commit_b[:24]}...")

        # Verify heights match
        if height_a != height_b:
            print(f"\nFAIL: Heights differ (A={height_a}, B={height_b})")
            return False

        # THE CRITICAL ASSERTION
        print("\n[6/6] Comparing commitments...")
        if commit_a == commit_b:
            print("\n" + "=" * 70)
            print("PASS: Cross-node Utreexo commitment is IDENTICAL")
            print("=" * 70)
            print(f"  Height:     {height_a}")
            print(f"  Commitment: {commit_a}")
            print("\nThis proves:")
            print("  - Accumulator computation is deterministic across processes")
            print("  - No hidden state affects commitment")
            print("  - Pool infrastructure with multiple daemons is safe")
            return True
        else:
            print("\n" + "=" * 70)
            print("FAIL: Cross-node Utreexo commitment DIFFERS!")
            print("=" * 70)
            print(f"  Node A: {commit_a}")
            print(f"  Node B: {commit_b}")
            print("\nThis is a CRITICAL BUG:")
            print("  - Accumulator computation is NONDETERMINISTIC")
            print("  - Nodes will diverge and reject each other's blocks")
            return False

    finally:
        print("\nCleaning up...")
        node_a.stop()
        node_b.stop()


def main():
    parser = argparse.ArgumentParser(
        description="Test cross-node Utreexo commitment determinism"
    )
    parser.add_argument(
        "--dinerod",
        default="./build/dinerod",
        help="Path to dinerod binary"
    )
    parser.add_argument(
        "--datadir",
        default="./data-cross-node-test",
        help="Base directory for test data"
    )
    parser.add_argument(
        "--address",
        default=DEFAULT_ADDRESS,
        help="Mining address for getblocktemplate"
    )
    parser.add_argument(
        "--keep-data",
        action="store_true",
        help="Keep test data after completion"
    )

    args = parser.parse_args()

    # Resolve paths
    dinerod = Path(args.dinerod).resolve()
    if not dinerod.exists():
        print(f"ERROR: dinerod not found at {dinerod}")
        sys.exit(1)

    base_datadir = Path(args.datadir).resolve()

    # Run test
    success = test_cross_node_determinism(str(dinerod), base_datadir, args.address)

    # Cleanup
    if not args.keep_data and base_datadir.exists():
        print(f"\nRemoving test data: {base_datadir}")
        shutil.rmtree(base_datadir)

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
