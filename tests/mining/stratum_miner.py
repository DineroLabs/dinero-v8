#!/usr/bin/env python3
"""
DineroCoin Stratum Pool Miner
Connects via Stratum V1 protocol for pool mining with 128-byte Utreexo headers.
"""

import argparse
import hashlib
import json
import random
import socket
import struct
import sys
import time
from typing import Optional, Tuple

class StratumMiner:
    """Stratum V1 pool miner for DineroCoin."""

    def __init__(self, pool_host: str, pool_port: int, wallet: str, worker: str = "1", password: str = "x"):
        self.pool_host = pool_host
        self.pool_port = pool_port
        self.wallet = wallet
        self.worker_name = f"{worker}"
        self.password = password

        self.sock: Optional[socket.socket] = None
        self.sock_file = None  # File object for line-based reading
        self.session_id: Optional[str] = None
        self.extranonce1: Optional[str] = None
        self.extranonce2_size: int = 4

        # Mining state
        self.job_id: Optional[str] = None
        self.prevhash: Optional[str] = None
        self.coinb1: Optional[str] = None
        self.coinb2: Optional[str] = None
        self.merkle_branches: list = []
        self.version: Optional[str] = None
        self.nbits: Optional[str] = None
        self.ntime: Optional[str] = None
        self.clean_jobs: bool = False

        # Utreexo extension
        self.utreexo_root: Optional[str] = None
        self.header_template: Optional[str] = None  # Full 128-byte template
        self.midstate: Optional[str] = None

        # Difficulty
        self.target: Optional[int] = None
        self.difficulty: float = 1.0

        # Stats
        self.shares_submitted = 0
        self.shares_accepted = 0
        self.shares_rejected = 0
        self.blocks_found = 0
        self.hash_count = 0
        self.start_time = time.time()

        self.msg_id = 0
        self.pending_requests = {}
        self.running = False
        self.verbose = True

    def log(self, msg: str):
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

    def connect(self) -> bool:
        """Connect to Stratum pool."""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(30)
            self.sock.connect((self.pool_host, self.pool_port))
            self.sock_file = self.sock.makefile('r', encoding='utf-8')
            self.log(f"Connected to {self.pool_host}:{self.pool_port}")
            return True
        except Exception as e:
            self.log(f"Connection failed: {e}")
            return False

    def send(self, method: str, params: list) -> int:
        """Send JSON-RPC request."""
        self.msg_id += 1
        msg = {
            "id": self.msg_id,
            "method": method,
            "params": params
        }
        data = json.dumps(msg) + "\n"
        self.sock.sendall(data.encode())
        self.pending_requests[self.msg_id] = method
        self.log(f"-> {method}: {params}")
        return self.msg_id

    def recv_line(self) -> Optional[dict]:
        """Receive a single JSON line."""
        try:
            line = self.sock_file.readline()
            if not line:
                return None
            line = line.strip()
            if line:
                return json.loads(line)
        except socket.timeout:
            return None
        except Exception as e:
            self.log(f"Recv error: {e}")
            return None
        return None

    def subscribe(self) -> bool:
        """Send mining.subscribe."""
        self.send("mining.subscribe", ["dinero-stratum-miner/1.0"])

        msg = self.recv_line()
        if not msg:
            return False

        self.log(f"<- {msg}")

        if "result" in msg and msg["result"]:
            result = msg["result"]
            # Result: [[[subscriptions]], extranonce1, extranonce2_size]
            if len(result) >= 3:
                self.extranonce1 = result[1]
                self.extranonce2_size = result[2]
                self.log(f"Subscribed: extranonce1={self.extranonce1}, en2_size={self.extranonce2_size}")
                return True

        return False

    def authorize(self) -> bool:
        """Send mining.authorize."""
        full_worker = f"{self.wallet}.{self.worker_name}"
        self.send("mining.authorize", [full_worker, self.password])

        # May receive difficulty notification first
        while True:
            msg = self.recv_line()
            if not msg:
                return False

            self.log(f"<- {msg}")

            if "method" in msg:
                self.handle_notification(msg)
            elif "result" in msg:
                return msg["result"] == True

        return False

    def handle_notification(self, msg: dict):
        """Handle pool notifications."""
        method = msg.get("method", "")
        params = msg.get("params", [])

        if method == "mining.set_difficulty":
            self.difficulty = params[0]
            self.target = self.difficulty_to_target(self.difficulty)
            self.log(f"Difficulty set to {self.difficulty}")

        elif method == "mining.notify":
            self.handle_job(params)

        elif method == "mining.set_extranonce":
            if len(params) >= 2:
                self.extranonce1 = params[0]
                self.extranonce2_size = params[1]

    def handle_job(self, params: list):
        """Handle mining.notify job."""
        # Standard: [job_id, prevhash, coinb1, coinb2, merkle, version, nbits, ntime, clean]
        # Extended: May include utreexo_root or header_template

        if len(params) < 9:
            self.log(f"Invalid job params: {len(params)} elements")
            return

        self.job_id = params[0]
        self.prevhash = params[1]
        self.coinb1 = params[2]
        self.coinb2 = params[3]
        self.merkle_branches = params[4]
        self.version = params[5]
        self.nbits = params[6]
        self.ntime = params[7]
        self.clean_jobs = params[8]

        # Check for Utreexo extensions
        if len(params) > 9:
            self.utreexo_root = params[9]
        if len(params) > 10:
            self.header_template = params[10]
        if len(params) > 11:
            self.midstate = params[11]

        self.log(f"New job: {self.job_id}, prevhash={self.prevhash[:16]}..., clean={self.clean_jobs}")

    def difficulty_to_target(self, diff: float) -> int:
        """Convert pool difficulty to target."""
        # Bitcoin difficulty 1 target
        diff1_target = 0x00000000FFFF0000000000000000000000000000000000000000000000000000
        return int(diff1_target / diff)

    def build_coinbase(self, extranonce2: bytes) -> bytes:
        """Build coinbase transaction."""
        coinb1 = bytes.fromhex(self.coinb1)
        coinb2 = bytes.fromhex(self.coinb2)
        en1 = bytes.fromhex(self.extranonce1)
        return coinb1 + en1 + extranonce2 + coinb2

    def merkle_root(self, coinbase: bytes) -> bytes:
        """Calculate merkle root from coinbase and branches."""
        # Double SHA256 of coinbase
        h = hashlib.sha256(hashlib.sha256(coinbase).digest()).digest()

        for branch in self.merkle_branches:
            branch_bytes = bytes.fromhex(branch)
            h = hashlib.sha256(hashlib.sha256(h + branch_bytes).digest()).digest()

        return h

    def build_header(self, merkle: bytes, ntime: int, nonce: int) -> bytes:
        """
        Build 128-byte block header (BlockHeader v1).

        Layout (matches include/mining/header_layout.h):
            0-3:     version (4 bytes, LE uint32)
            4-35:    prev_block_hash (32 bytes)
            36-67:   merkle_root (32 bytes)
            68-99:   utreexo_root (32 bytes)
            100-107: timestamp (8 bytes, LE uint64)
            108-111: difficulty (4 bytes, LE uint32)
            112-115: nonce (4 bytes, LE uint32)
            116-127: reserved (12 bytes, must be zeros)
        """
        # If we have a prebuilt 128-byte template, use it
        if self.header_template:
            header = bytearray.fromhex(self.header_template)
            # Insert nonce at offset 112
            struct.pack_into('<I', header, 112, nonce)
            return bytes(header)

        # Build manually — BlockHeader v1 (128 bytes)
        version = struct.pack('<I', int(self.version, 16))
        prevhash = bytes.fromhex(self.prevhash)  # Already internal byte order from Stratum
        nbits = struct.pack('<I', int(self.nbits, 16))
        ntime_bytes = struct.pack('<Q', ntime)  # uint64 timestamp
        nonce_bytes = struct.pack('<I', nonce)

        if self.utreexo_root:
            utreexo = bytes.fromhex(self.utreexo_root)
        else:
            utreexo = b'\x00' * 32

        reserved = b'\x00' * 12

        # version(4) + prevhash(32) + merkle(32) + utreexo(32) +
        # timestamp(8) + nbits(4) + nonce(4) + reserved(12) = 128
        header = version + prevhash + merkle + utreexo + ntime_bytes + nbits + nonce_bytes + reserved

        assert len(header) == 128, f"Header must be 128 bytes, got {len(header)}"
        return header

    def hash_header(self, header: bytes) -> bytes:
        """Double SHA256 hash of header."""
        return hashlib.sha256(hashlib.sha256(header).digest()).digest()

    def hash_to_int(self, h: bytes) -> int:
        """Convert hash to integer (little-endian)."""
        return int.from_bytes(h, 'little')

    def mine_share(self, max_nonces: int = 0x100000) -> Optional[Tuple[int, int, bytes]]:
        """Mine for a valid share. Returns (nonce, ntime, hash) or None."""
        if not self.job_id:
            return None

        # Generate random extranonce2
        extranonce2 = random.randbytes(self.extranonce2_size)

        # Build coinbase and merkle root
        coinbase = self.build_coinbase(extranonce2)
        merkle = self.merkle_root(coinbase)

        ntime = int(self.ntime, 16)
        start_nonce = random.randint(0, 0xFFFFFFFF - max_nonces)

        for i in range(max_nonces):
            nonce = (start_nonce + i) & 0xFFFFFFFF
            header = self.build_header(merkle, ntime, nonce)
            h = self.hash_header(header)
            hash_int = self.hash_to_int(h)

            self.hash_count += 1

            if hash_int <= self.target:
                return (nonce, ntime, extranonce2, h)

        return None

    def submit_share(self, nonce: int, ntime: int, extranonce2: bytes) -> bool:
        """Submit share to pool."""
        worker = f"{self.wallet}.{self.worker_name}"
        en2_hex = extranonce2.hex()
        ntime_hex = f"{ntime:08x}"
        nonce_hex = f"{nonce:08x}"

        self.send("mining.submit", [worker, self.job_id, en2_hex, ntime_hex, nonce_hex])
        self.shares_submitted += 1

        # Wait for response
        msg = self.recv_line()
        if msg:
            self.log(f"<- {msg}")

            if "method" in msg:
                self.handle_notification(msg)
                # Get actual response
                msg = self.recv_line()
                if msg:
                    self.log(f"<- {msg}")

            if msg and "result" in msg:
                if msg["result"] == True:
                    self.shares_accepted += 1
                    self.log("Share ACCEPTED!")
                    return True
                else:
                    self.shares_rejected += 1
                    error = msg.get("error", "Unknown")
                    self.log(f"Share REJECTED: {error}")

        return False

    def print_stats(self):
        """Print mining statistics."""
        elapsed = time.time() - self.start_time
        hashrate = self.hash_count / elapsed if elapsed > 0 else 0

        print("\n" + "=" * 50)
        print("Mining Statistics")
        print("=" * 50)
        print(f"Hashrate:        {hashrate:.2f} H/s")
        print(f"Total hashes:    {self.hash_count:,}")
        print(f"Runtime:         {elapsed:.1f}s")
        print(f"Shares submitted: {self.shares_submitted}")
        print(f"Shares accepted:  {self.shares_accepted}")
        print(f"Shares rejected:  {self.shares_rejected}")
        print(f"Blocks found:     {self.blocks_found}")
        print(f"Difficulty:       {self.difficulty}")
        print("=" * 50 + "\n")

    def run(self, duration: int = 60, target_shares: int = 0):
        """Main mining loop."""
        if not self.connect():
            return False

        if not self.subscribe():
            self.log("Subscribe failed")
            return False

        if not self.authorize():
            self.log("Authorize failed")
            return False

        # Wait for initial job
        self.log("Waiting for mining job...")
        while not self.job_id:
            msg = self.recv_line()
            if msg:
                self.log(f"<- {msg}")
                if "method" in msg:
                    self.handle_notification(msg)

        if not self.target:
            self.target = self.difficulty_to_target(self.difficulty)

        self.log(f"Starting mining at difficulty {self.difficulty}")
        self.running = True
        self.start_time = time.time()

        try:
            while self.running:
                # Check stop conditions
                elapsed = time.time() - self.start_time
                if duration > 0 and elapsed >= duration:
                    self.log(f"Duration {duration}s reached")
                    break

                if target_shares > 0 and self.shares_accepted >= target_shares:
                    self.log(f"Target {target_shares} shares reached")
                    break

                # Mine for a share
                result = self.mine_share(max_nonces=100000)

                if result:
                    nonce, ntime, extranonce2, h = result
                    hash_hex = h[::-1].hex()
                    self.log(f"Found share! nonce={nonce:08x}, hash={hash_hex[:16]}...")
                    self.submit_share(nonce, ntime, extranonce2)

                # Check for new jobs (non-blocking)
                self.sock.setblocking(False)
                try:
                    msg = self.recv_line()
                    if msg and "method" in msg:
                        self.handle_notification(msg)
                except:
                    pass
                finally:
                    self.sock.setblocking(True)
                    self.sock.settimeout(30)

        except KeyboardInterrupt:
            self.log("Interrupted by user")

        finally:
            self.running = False
            self.print_stats()
            if self.sock:
                self.sock.close()

        return self.shares_accepted > 0


def main():
    parser = argparse.ArgumentParser(description="DineroCoin Stratum Pool Miner")
    parser.add_argument("--pool", default="127.0.0.1:3333", help="Pool address (host:port)")
    parser.add_argument("--wallet", required=True, help="Wallet address for payouts")
    parser.add_argument("--worker", default="1", help="Worker name/ID")
    parser.add_argument("--password", default="x", help="Worker password")
    parser.add_argument("--duration", type=int, default=60, help="Mining duration in seconds (0=unlimited)")
    parser.add_argument("--shares", type=int, default=0, help="Stop after N accepted shares")
    parser.add_argument("--quiet", action="store_true", help="Reduce output")

    args = parser.parse_args()

    # Parse pool address
    if ":" in args.pool:
        host, port = args.pool.rsplit(":", 1)
        port = int(port)
    else:
        host = args.pool
        port = 3333

    miner = StratumMiner(
        pool_host=host,
        pool_port=port,
        wallet=args.wallet,
        worker=args.worker,
        password=args.password
    )
    miner.verbose = not args.quiet

    print("=" * 50)
    print("DineroCoin Stratum Pool Miner")
    print("=" * 50)
    print(f"Pool:     {host}:{port}")
    print(f"Wallet:   {args.wallet}")
    print(f"Worker:   {args.worker}")
    print(f"Duration: {args.duration}s")
    print("=" * 50)
    print()

    success = miner.run(duration=args.duration, target_shares=args.shares)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
