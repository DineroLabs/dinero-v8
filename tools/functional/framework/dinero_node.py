"""
DineroCoin Node Management for Functional Tests

Manages dinerod processes for testing.
"""

import os
import subprocess
import tempfile
import shutil
import time
import signal
import random
import string
from pathlib import Path
from typing import Optional, List, Dict, Any

from .rpc import DineroRPC, RPCError


class DineroNode:
    """
    Manages a single dinerod instance for testing.

    Usage:
        node = DineroNode(0, datadir="/tmp/dinero_test_0", rpcport=20996)
        node.start()
        print(node.getblockcount())
        node.stop()
    """

    # Path to dinerod binary (set by test framework)
    DINEROD_PATH: str = ""

    def __init__(
        self,
        index: int,
        datadir: str,
        rpcport: int,
        p2pport: int,
        network: str = "regtest",
        extra_args: Optional[List[str]] = None,
    ):
        self.index = index
        self.datadir = Path(datadir)
        self.rpcport = rpcport
        self.p2pport = p2pport
        self.network = network
        self.extra_args = extra_args or []

        # Generate random RPC credentials
        self.rpcuser = f"dinero_test_{index}"
        self.rpcpassword = ''.join(random.choices(string.ascii_letters + string.digits, k=32))

        # Process handle
        self.process: Optional[subprocess.Popen] = None
        self.running = False

        # RPC client (initialized after start)
        self._rpc: Optional[DineroRPC] = None

    @property
    def rpc(self) -> DineroRPC:
        """Get RPC client, initializing if needed"""
        if self._rpc is None:
            self._rpc = DineroRPC(
                url=f"http://127.0.0.1:{self.rpcport}",
                user=self.rpcuser,
                password=self.rpcpassword,
            )
        return self._rpc

    def __getattr__(self, name: str):
        """Proxy RPC calls directly through the node object"""
        # Avoid infinite recursion for internal attributes
        if name.startswith('_') or name in ('rpc', 'process', 'running', 'datadir',
                                              'rpcport', 'p2pport', 'index', 'network',
                                              'rpcuser', 'rpcpassword', 'extra_args'):
            raise AttributeError(f"'{type(self).__name__}' object has no attribute '{name}'")
        return getattr(self.rpc, name)

    def setup_datadir(self) -> None:
        """Create datadir and config file"""
        self.datadir.mkdir(parents=True, exist_ok=True)

        # Write dinero.conf
        # Use cookie auth (don't set rpcuser/rpcpassword)
        conf_path = self.datadir / "dinero.conf"
        conf_content = f"""# DineroCoin functional test config
{self.network}=1
server=1
daemon=0
txindex=1
rpcport={self.rpcport}
port={self.p2pport}
rpcallowip=127.0.0.1
rpcbind=127.0.0.1
listenonion=0
discover=0
dnsseed=0
fixedseeds=0
printtoconsole=0
"""
        conf_path.write_text(conf_content)

    def start(self, extra_args: Optional[List[str]] = None) -> None:
        """Start the dinerod process"""
        # Check if process is actually running (not just the flag)
        if self.process is not None and self.process.poll() is None:
            return  # Already running

        # Reset state if process died
        if self.running and (self.process is None or self.process.poll() is not None):
            self.running = False
            self.process = None
            self._rpc = None

        if not DineroNode.DINEROD_PATH:
            raise RuntimeError("DineroNode.DINEROD_PATH not set")

        if not os.path.exists(DineroNode.DINEROD_PATH):
            raise RuntimeError(f"dinerod not found at {DineroNode.DINEROD_PATH}")

        self.setup_datadir()

        args = [
            DineroNode.DINEROD_PATH,
            f"-datadir={self.datadir}",
            f"-{self.network}",
            f"-rpcport={self.rpcport}",
            f"-port={self.p2pport}",
        ]

        # Add extra args
        args.extend(self.extra_args)
        if extra_args:
            args.extend(extra_args)

        # Start process
        stdout_path = self.datadir / "stdout.log"
        stderr_path = self.datadir / "stderr.log"

        with open(stdout_path, 'w') as stdout_file, open(stderr_path, 'w') as stderr_file:
            self.process = subprocess.Popen(
                args,
                stdout=stdout_file,
                stderr=stderr_file,
                cwd=str(self.datadir),
            )

        self.running = True

        # Wait for cookie file to appear, then read credentials
        # Cookie can be at datadir root or in network subdirectory
        cookie_paths = [
            self.datadir / ".cookie",
            self.datadir / self.network / ".cookie",
        ]
        for _ in range(120):  # Wait up to 60 seconds
            for cookie_path in cookie_paths:
                if cookie_path.exists():
                    try:
                        cookie = cookie_path.read_text().strip()
                        if ":" in cookie:
                            self.rpcuser, self.rpcpassword = cookie.split(":", 1)
                            self._rpc = None  # Reset RPC client with new credentials
                            break
                    except:
                        pass
            if self.rpcuser == "__cookie__":
                break
            time.sleep(0.5)

        # Wait for RPC to be ready
        if not self.rpc.wait_for_rpc_connection(timeout=60):
            self.stop()
            # Read stderr for debugging
            stderr_content = stderr_path.read_text() if stderr_path.exists() else ""
            raise RuntimeError(f"Node {self.index} failed to start. stderr:\n{stderr_content}")

    def stop(self, wait: bool = True) -> None:
        """Stop the dinerod process"""
        if not self.running or self.process is None:
            return

        try:
            # Try graceful shutdown via RPC
            self.rpc.stop()
        except:
            pass

        if wait:
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                # Force kill
                self.process.kill()
                self.process.wait()
        else:
            self.process.terminate()

        self.running = False
        self.process = None
        self._rpc = None
        # Reset cookie credentials so start() will read new cookie on restart
        self.rpcuser = f"dinero_test_{self.index}"
        self.rpcpassword = ""

    def cleanup(self) -> None:
        """Stop node and remove datadir"""
        self.stop()
        if self.datadir.exists():
            shutil.rmtree(self.datadir)

    def wait_for_block(self, height: int, timeout: int = 60) -> bool:
        """Wait for node to reach specified block height"""
        start = time.time()
        while time.time() - start < timeout:
            try:
                if self.getblockcount() >= height:
                    return True
            except:
                pass
            time.sleep(0.5)
        return False

    def wait_for_blockhash(self, blockhash: str, timeout: int = 60) -> bool:
        """Wait for node to have a specific block"""
        start = time.time()
        while time.time() - start < timeout:
            try:
                self.getblock(blockhash)
                return True
            except RPCError:
                pass
            time.sleep(0.5)
        return False

    def is_running(self) -> bool:
        """Check if node process is still running"""
        if self.process is None:
            return False
        return self.process.poll() is None

    def read_log(self, lines: int = 100) -> str:
        """Read last N lines from debug.log"""
        log_path = self.datadir / self.network / "debug.log"
        if not log_path.exists():
            return ""
        content = log_path.read_text()
        return '\n'.join(content.split('\n')[-lines:])

    def __repr__(self) -> str:
        status = "running" if self.running else "stopped"
        return f"<DineroNode {self.index} rpc={self.rpcport} p2p={self.p2pport} [{status}]>"
