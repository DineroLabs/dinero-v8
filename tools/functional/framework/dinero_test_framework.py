"""
DineroCoin Test Framework

Base class for functional tests. Manages node lifecycle, provides utilities.
Inspired by Bitcoin Core's test framework, tailored to DineroCoin.
"""

import os
import sys
import tempfile
import shutil
import time
import traceback
import argparse
from pathlib import Path
from typing import List, Optional, Dict, Any

from .dinero_node import DineroNode
from .rpc import DineroRPC, RPCError
from .util import (
    connect_nodes,
    sync_blocks,
    sync_mempools,
    sync_all,
    assert_equal,
    wait_until,
)


class SkipTest(Exception):
    """Raise to skip a test"""
    pass


class DineroTestFramework:
    """
    Base class for DineroCoin functional tests.

    Usage:
        class MyTest(DineroTestFramework):
            def set_test_params(self):
                self.num_nodes = 2

            def run_test(self):
                self.nodes[0].generate(10)
                sync_blocks(self.nodes)
                assert_equal(self.nodes[1].getblockcount(), 10)

        if __name__ == '__main__':
            MyTest().main()
    """

    # Override in subclass
    def set_test_params(self):
        """
        Override to set test parameters before setup.

        Set self.num_nodes, self.extra_args, etc.
        """
        self.num_nodes = 1

    def setup_network(self):
        """
        Override to customize network setup.

        Default: start all nodes and connect them in a chain.
        """
        self.setup_nodes()
        if self.num_nodes > 1:
            self.connect_all_nodes()

    def run_test(self):
        """
        Override with actual test logic.

        This is where your test lives.
        """
        raise NotImplementedError("Subclass must implement run_test()")

    # Framework implementation

    def __init__(self):
        self.nodes: List[DineroNode] = []
        self.num_nodes: int = 1
        self.network: str = "regtest"
        self.extra_args: List[List[str]] = []  # Per-node extra args
        self.supports_cli: bool = False

        # Paths
        self.options: argparse.Namespace = argparse.Namespace()
        self.tmpdir: Optional[Path] = None
        self.dinerod_path: str = ""

        # Ports (auto-assigned)
        self._next_rpc_port: int = 18400
        self._next_p2p_port: int = 18500

    def _get_next_ports(self) -> tuple:
        """Get next available RPC and P2P ports"""
        rpc = self._next_rpc_port
        p2p = self._next_p2p_port
        self._next_rpc_port += 1
        self._next_p2p_port += 1
        return rpc, p2p

    def add_options(self, parser: argparse.ArgumentParser) -> None:
        """Override to add test-specific options"""
        pass

    def parse_args(self) -> None:
        """Parse command line arguments"""
        parser = argparse.ArgumentParser(description=self.__doc__ or "DineroCoin functional test")

        parser.add_argument(
            "--dinerod",
            dest="dinerod",
            default="",
            help="Path to dinerod binary"
        )
        parser.add_argument(
            "--tmpdir",
            dest="tmpdir",
            default="",
            help="Root directory for test datadirs"
        )
        parser.add_argument(
            "--nocleanup",
            dest="nocleanup",
            action="store_true",
            help="Don't clean up test directory on exit"
        )
        parser.add_argument(
            "--timeout",
            dest="timeout",
            type=int,
            default=60,
            help="Timeout for individual operations (default: 60s)"
        )
        parser.add_argument(
            "-v", "--verbose",
            dest="verbose",
            action="store_true",
            help="Verbose output"
        )

        self.add_options(parser)
        self.options = parser.parse_args()

    def setup(self) -> None:
        """Setup test environment"""
        self.parse_args()

        # Find dinerod binary
        if self.options.dinerod:
            self.dinerod_path = self.options.dinerod
        else:
            # Look in common locations
            candidates = [
                "./build/dinerod",
                "../build/dinerod",
                "../../build/dinerod",
                os.path.expanduser("~/Documents/DineroCoin/build/dinerod"),
            ]
            for path in candidates:
                if os.path.exists(path):
                    self.dinerod_path = os.path.abspath(path)
                    break

        if not self.dinerod_path or not os.path.exists(self.dinerod_path):
            raise RuntimeError(
                f"dinerod not found. Use --dinerod=/path/to/dinerod or build first."
            )

        DineroNode.DINEROD_PATH = self.dinerod_path

        # Create temp directory
        if self.options.tmpdir:
            self.tmpdir = Path(self.options.tmpdir)
            self.tmpdir.mkdir(parents=True, exist_ok=True)
        else:
            self.tmpdir = Path(tempfile.mkdtemp(prefix="dinero_test_"))

        self.log(f"Test directory: {self.tmpdir}")
        self.log(f"Using dinerod: {self.dinerod_path}")

    def setup_nodes(self) -> None:
        """Create and start all nodes"""
        for i in range(self.num_nodes):
            rpc_port, p2p_port = self._get_next_ports()

            extra = []
            if i < len(self.extra_args):
                extra = self.extra_args[i]

            node = DineroNode(
                index=i,
                datadir=str(self.tmpdir / f"node{i}"),
                rpcport=rpc_port,
                p2pport=p2p_port,
                network=self.network,
                extra_args=extra,
            )

            self.nodes.append(node)
            node.start()
            self.log(f"Started node {i} (rpc={rpc_port}, p2p={p2p_port})")

    def connect_all_nodes(self) -> None:
        """Connect nodes in a chain: 0-1-2-3..."""
        for i in range(len(self.nodes) - 1):
            connect_nodes(self.nodes[i], self.nodes[i + 1])
        self.log(f"Connected {len(self.nodes)} nodes in chain")

    def stop_nodes(self) -> None:
        """Stop all nodes"""
        for node in self.nodes:
            try:
                node.stop()
            except Exception as e:
                self.log(f"Warning: Error stopping node {node.index}: {e}")

    def cleanup(self) -> None:
        """Clean up test environment"""
        self.stop_nodes()

        if not self.options.nocleanup and self.tmpdir and self.tmpdir.exists():
            try:
                shutil.rmtree(self.tmpdir)
            except Exception as e:
                self.log(f"Warning: Could not clean up {self.tmpdir}: {e}")

    def log(self, msg: str) -> None:
        """Log a message"""
        print(f"[TEST] {msg}")

    def skip(self, reason: str) -> None:
        """Skip this test"""
        raise SkipTest(reason)

    # Convenience methods

    def sync_blocks(self, nodes: Optional[List[DineroNode]] = None, timeout: int = 60) -> None:
        """Sync blocks across nodes"""
        sync_blocks(nodes or self.nodes, timeout)

    def sync_mempools(self, nodes: Optional[List[DineroNode]] = None, timeout: int = 60) -> None:
        """Sync mempools across nodes"""
        sync_mempools(nodes or self.nodes, timeout)

    def sync_all(self, nodes: Optional[List[DineroNode]] = None, timeout: int = 60) -> None:
        """Sync blocks and mempools"""
        sync_all(nodes or self.nodes, timeout)

    def generate_and_sync(self, node_index: int, count: int) -> List[str]:
        """Mine blocks on a node and sync all nodes"""
        hashes = self.nodes[node_index].generate(count)
        self.sync_blocks()
        return hashes

    # Main entry point

    def main(self) -> None:
        """Run the test"""
        exit_code = 0

        try:
            self.set_test_params()
            self.setup()
            self.setup_network()

            self.log("=" * 60)
            self.log(f"Running: {self.__class__.__name__}")
            self.log("=" * 60)

            self.run_test()

            self.log("=" * 60)
            self.log("PASSED")
            self.log("=" * 60)

        except SkipTest as e:
            self.log(f"SKIPPED: {e}")
            exit_code = 77  # Standard skip code

        except Exception as e:
            self.log("=" * 60)
            self.log(f"FAILED: {e}")
            self.log("=" * 60)
            traceback.print_exc()
            exit_code = 1

        finally:
            self.cleanup()

        sys.exit(exit_code)
