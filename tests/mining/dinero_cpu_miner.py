#!/usr/bin/env python3
"""
DineroCoin CPU Miner - Reference Implementation
================================================

A complete CPU miner for DineroCoin that demonstrates:
1. RPC authentication via .cookie file
2. getblocktemplate request/response handling
3. Block header construction (128 bytes)
4. SHA256d proof-of-work hashing
5. Target comparison
6. Block submission via submitblock
7. Daemon-owned coinbasetxn consumption

This is a reference implementation for external miners.

Usage:
    python3 dinero_cpu_miner.py --rpc-url http://127.0.0.1:22020 --address <mining_address>
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request
import urllib.error
from typing import Dict, Any, Optional, Tuple, List
from dataclasses import dataclass


@dataclass
class BlockTemplate:
    """Block template from getblocktemplate RPC"""
    version: int
    height: int
    previous_block_hash: str
    bits: str
    curtime: int
    mintime: int
    maxtime: int
    coinbase_value: int
    transactions: List[Dict[str, Any]]
    coinbase_tx_hex: str = ""
    coinbase_txid: str = ""
    utreexo_commitment: str = ""
    target: str = ""

    def __post_init__(self):
        """Calculate target from bits"""
        if not self.target:
            self.target = self.bits_to_target(self.bits)

    @staticmethod
    def bits_to_target(bits_hex: str) -> str:
        """Convert compact bits to 256-bit target"""
        bits = int(bits_hex, 16)
        exponent = bits >> 24
        mantissa = bits & 0xffffff
        target = mantissa * (2 ** (8 * (exponent - 3)))
        return format(target, '064x')


class DineroCoinMiner:
    """
    DineroCoin CPU Miner

    Implements the full mining workflow:
    1. Connect to daemon via JSON-RPC
    2. Request block template
    3. Construct block header
    4. Hash with SHA256d
    5. Submit solved blocks
    """

    def __init__(self, rpc_url: str, cookie_path: str = None, mining_address: str = None):
        self.rpc_url = rpc_url
        self.cookie_path = cookie_path
        self.mining_address = mining_address
        self.rpc_auth = None
        self.total_hashes = 0
        self.blocks_found = 0
        self.start_time = time.time()
        self.last_mining_result: Dict[str, Any] = {}

    def load_cookie_auth(self) -> bool:
        """Load RPC authentication from .cookie file"""
        if not self.cookie_path:
            # Auto-detect common paths
            possible_paths = [
                os.path.expanduser("~/.dinero/regtest/.cookie"),
                os.path.expanduser("~/.dinero/.cookie"),
                "/tmp/din_final/.cookie",
            ]
            for path in possible_paths:
                if os.path.exists(path):
                    self.cookie_path = path
                    break

        if not self.cookie_path or not os.path.exists(self.cookie_path):
            print(f"[ERROR] Cookie file not found. Tried: {self.cookie_path or 'auto-detection'}")
            return False

        try:
            with open(self.cookie_path, 'r') as f:
                self.rpc_auth = f.read().strip()
            print(f"[INFO] Loaded RPC auth from {self.cookie_path}")
            return True
        except Exception as e:
            print(f"[ERROR] Failed to read cookie: {e}")
            return False

    def rpc_call(self, method: str, params: List[Any] = None) -> Dict[str, Any]:
        """Make JSON-RPC call to dinerod"""
        if params is None:
            params = []

        payload = {
            "jsonrpc": "2.0",
            "id": int(time.time() * 1000),
            "method": method,
            "params": params
        }

        headers = {
            "Content-Type": "application/json",
        }

        # Add authentication if available
        if self.rpc_auth:
            import base64
            auth_bytes = base64.b64encode(self.rpc_auth.encode()).decode()
            headers["Authorization"] = f"Basic {auth_bytes}"

        data = json.dumps(payload).encode('utf-8')

        try:
            req = urllib.request.Request(self.rpc_url, data=data, headers=headers, method='POST')
            with urllib.request.urlopen(req, timeout=30) as response:
                result = json.loads(response.read().decode('utf-8'))
                if "error" in result and result["error"]:
                    raise Exception(f"RPC error: {result['error']}")
                return result.get("result", result)
        except urllib.error.HTTPError as e:
            body = e.read().decode('utf-8') if e.fp else ""
            raise Exception(f"HTTP {e.code}: {body}")
        except urllib.error.URLError as e:
            raise Exception(f"Connection failed: {e.reason}")

    def get_block_template(self) -> Optional[BlockTemplate]:
        """Request block template from daemon"""
        try:
            # Request template with segwit rules
            params = {"rules": ["segwit"]}
            if self.mining_address:
                params["address"] = self.mining_address
            result = self.rpc_call("getblocktemplate", [params])

            if "error" in result:
                print(f"[ERROR] getblocktemplate failed: {result['error']}")
                return None

            coinbase_txn = result.get("coinbasetxn", {}) or {}
            utreexo_obj = result.get("utreexo", {}) or {}

            return BlockTemplate(
                version=result.get("version", 1),
                height=result["height"],
                previous_block_hash=result["previousblockhash"],
                bits=result["bits"],
                curtime=result["curtime"],
                mintime=result.get("mintime", result["curtime"]),
                maxtime=result.get("maxtime", result["curtime"] + 7200),
                coinbase_value=result["coinbasevalue"],
                transactions=result.get("transactions", []),
                coinbase_tx_hex=coinbase_txn.get("data", ""),
                coinbase_txid=coinbase_txn.get("txid", ""),
                utreexo_commitment=utreexo_obj.get("commitment", result.get("utreexocommitment", "")),
                target=result.get("target", ""),
            )
        except Exception as e:
            print(f"[ERROR] Failed to get block template: {e}")
            return None

    def create_coinbase_tx(self, height: int, value: int, address: str) -> bytes:
        """
        Create coinbase transaction

        Format:
        - version (4 bytes, LE)
        - input count (varint)
        - coinbase input:
          - prevout hash (32 bytes, zeros)
          - prevout index (4 bytes, 0xffffffff)
          - script length (varint)
          - coinbase script (includes height BIP34)
          - sequence (4 bytes, 0xffffffff)
        - output count (varint)
        - output:
          - value (8 bytes, LE)
          - script length (varint)
          - scriptPubKey
        - locktime (4 bytes, zeros)
        """
        tx = bytearray()

        # Version (1 for standard)
        tx.extend(struct.pack('<I', 1))

        # Input count (1)
        tx.append(1)

        # Coinbase input
        tx.extend(b'\x00' * 32)  # prevout hash (null)
        tx.extend(struct.pack('<I', 0xffffffff))  # prevout index

        # Coinbase script (BIP34: height + extranonce)
        coinbase_script = self._make_coinbase_script(height)
        tx.append(len(coinbase_script))
        tx.extend(coinbase_script)

        tx.extend(struct.pack('<I', 0xffffffff))  # sequence

        # Output count (1)
        tx.append(1)

        # Output value
        tx.extend(struct.pack('<Q', value))

        # Output scriptPubKey
        script_pubkey = self._address_to_script_pubkey(address)
        tx.append(len(script_pubkey))
        tx.extend(script_pubkey)

        # Locktime
        tx.extend(struct.pack('<I', 0))

        return bytes(tx)

    def _make_coinbase_script(self, height: int) -> bytes:
        """Create BIP34 coinbase script with height"""
        script = bytearray()

        # BIP34: Push height
        if height < 17:
            script.append(0x50 + height)  # OP_1 to OP_16
        elif height < 256:
            script.append(1)
            script.append(height)
        elif height < 65536:
            script.append(2)
            script.extend(struct.pack('<H', height))
        elif height < 16777216:
            script.append(3)
            script.extend(struct.pack('<I', height)[:3])
        else:
            script.append(4)
            script.extend(struct.pack('<I', height))

        # Add miner tag
        miner_tag = b'/DineroCoin CPU Miner/'
        script.append(len(miner_tag))
        script.extend(miner_tag)

        return bytes(script)

    def _address_to_script_pubkey(self, address: str) -> bytes:
        """Convert address to scriptPubKey"""
        # Handle bech32 addresses (P2WPKH)
        if address.startswith('din1') or address.startswith('tdr1') or address.startswith('bcrt1'):
            # Decode bech32
            decoded = self._bech32_decode(address)
            if decoded:
                # P2WPKH: OP_0 <20 bytes>
                script = bytearray()
                script.append(0x00)  # OP_0 (witness version)
                script.append(len(decoded))
                script.extend(decoded)
                return bytes(script)

        # Fallback: return OP_TRUE for testing
        return bytes([0x51])  # OP_TRUE

    def _bech32_decode(self, address: str) -> Optional[bytes]:
        """Decode bech32 address to witness program"""
        CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"

        # Split HRP and data
        pos = address.rfind('1')
        if pos < 1 or pos + 7 > len(address):
            return None

        data_part = address[pos+1:].lower()

        # Decode data
        decoded = []
        for c in data_part:
            if c not in CHARSET:
                return None
            decoded.append(CHARSET.index(c))

        # Convert from 5-bit to 8-bit (skip checksum and version)
        if len(decoded) < 7:
            return None

        version = decoded[0]
        data = decoded[1:-6]  # Skip version and 6-char checksum

        # Convert 5-bit to 8-bit
        result = []
        acc = 0
        bits = 0
        for v in data:
            acc = (acc << 5) | v
            bits += 5
            while bits >= 8:
                bits -= 8
                result.append((acc >> bits) & 0xff)

        return bytes(result)

    def build_block_header(self, template: BlockTemplate,
                          merkle_root: str, nonce: int, timestamp: int) -> bytes:
        """
        Build 128-byte Dinero block header

        Format:
        - version (4 bytes, LE)
        - previous block hash (32 bytes, internal byte order)
        - merkle root (32 bytes, internal byte order)
        - utreexo root (32 bytes, internal byte order)
        - timestamp (8 bytes, LE)
        - bits (4 bytes, LE)
        - nonce (4 bytes, LE)
        - reserved (12 bytes, zeros)
        """
        header = bytearray()

        # Version
        header.extend(struct.pack('<I', template.version))

        # Previous block hash (reverse byte order for internal representation)
        prev_hash_bytes = bytes.fromhex(template.previous_block_hash)
        header.extend(prev_hash_bytes[::-1])

        # Merkle root (reverse byte order)
        merkle_bytes = bytes.fromhex(merkle_root)
        header.extend(merkle_bytes[::-1])

        # Utreexo commitment (reverse byte order into header internal layout)
        if len(template.utreexo_commitment) != 64:
            raise ValueError("Template is missing a 32-byte utreexo commitment")
        utreexo_bytes = bytes.fromhex(template.utreexo_commitment)
        header.extend(utreexo_bytes[::-1])

        # Timestamp
        header.extend(struct.pack('<Q', timestamp))

        # Bits (compact target)
        header.extend(struct.pack('<I', int(template.bits, 16)))

        # Nonce
        header.extend(struct.pack('<I', nonce))

        # Reserved field (must be zero)
        header.extend(b'\x00' * 12)

        return bytes(header)

    def sha256d(self, data: bytes) -> bytes:
        """Double SHA-256 hash"""
        return hashlib.sha256(hashlib.sha256(data).digest()).digest()

    def hash_to_hex(self, hash_bytes: bytes) -> str:
        """Convert hash to display hex (reversed)"""
        return hash_bytes[::-1].hex()

    def compute_merkle_root(self, tx_hashes: List[bytes]) -> bytes:
        """Compute merkle root from transaction hashes"""
        if not tx_hashes:
            return bytes(32)

        # Work with copies
        hashes = list(tx_hashes)

        while len(hashes) > 1:
            # Pad to even number
            if len(hashes) % 2 == 1:
                hashes.append(hashes[-1])

            # Pairwise hash
            new_hashes = []
            for i in range(0, len(hashes), 2):
                combined = hashes[i] + hashes[i + 1]
                new_hashes.append(self.sha256d(combined))
            hashes = new_hashes

        return hashes[0]

    def check_pow(self, header_hash: bytes, target: str) -> bool:
        """Check if hash meets target"""
        hash_int = int.from_bytes(header_hash, 'big')
        target_int = int(target, 16)
        return hash_int <= target_int

    def get_coinbase_tx(self, template: BlockTemplate) -> bytes:
        """Use daemon-provided coinbasetxn when available."""
        if template.coinbase_tx_hex:
            return bytes.fromhex(template.coinbase_tx_hex)

        if not self.mining_address:
            raise ValueError("Template missing coinbasetxn and no mining address was provided")

        print("[WARN] Template missing coinbasetxn, falling back to local coinbase construction")
        return self.create_coinbase_tx(
            template.height,
            template.coinbase_value,
            self.mining_address
        )

    def mine_block(self, template: BlockTemplate, max_nonce: int = 0xffffffff) -> Optional[Tuple[bytes, int]]:
        """
        Mine a block from template

        Returns (block_bytes, nonce) on success, None on failure
        """
        # Use the daemon-owned coinbase exactly when provided.
        coinbase_tx = self.get_coinbase_tx(template)
        if template.coinbase_txid:
            coinbase_hash = bytes.fromhex(template.coinbase_txid)[::-1]
        else:
            coinbase_hash = self.sha256d(coinbase_tx)

        # Build merkle tree with coinbase + transactions
        tx_hashes = [coinbase_hash]
        for tx in template.transactions:
            tx_hash = bytes.fromhex(tx["txid"])[::-1]  # txid is already display order
            tx_hashes.append(tx_hash)

        merkle_root = self.compute_merkle_root(tx_hashes)
        merkle_root_hex = merkle_root[::-1].hex()

        # Timestamp is miner-owned within the daemon-provided window.
        timestamp = max(template.curtime, template.mintime, int(time.time()))
        if template.maxtime:
            timestamp = min(timestamp, template.maxtime)

        print(f"[INFO] Mining block {template.height}")
        print(f"  Previous: {template.previous_block_hash[:16]}...")
        print(f"  Target:   {template.target[:16]}...")
        print(f"  Bits:     {template.bits}")
        print(f"  Merkle:   {merkle_root_hex[:16]}...")
        if template.coinbase_txid:
            print(f"  Coinbase: {template.coinbase_txid[:16]}...")

        # Hash loop
        start_time = time.time()
        nonce = 0
        report_interval = 100000

        while nonce <= max_nonce:
            # Build header
            header = self.build_block_header(template, merkle_root_hex, nonce, timestamp)

            # Hash
            header_hash = self.sha256d(header)
            self.total_hashes += 1

            # Check target
            if self.check_pow(header_hash, template.target):
                elapsed = time.time() - start_time
                hashrate = self.total_hashes / max(elapsed, 0.001)

                print(f"\n[SUCCESS] Block found!")
                print(f"  Hash:     {self.hash_to_hex(header_hash)}")
                print(f"  Nonce:    {nonce}")
                print(f"  Hashrate: {hashrate/1000:.2f} kH/s")

                # Build full block
                block = self.build_full_block(header, coinbase_tx, template.transactions)
                self.last_mining_result = {
                    "template_height": template.height,
                    "template_coinbasetxn_present": bool(template.coinbase_tx_hex),
                    "template_coinbase_hex": coinbase_tx.hex(),
                    "template_coinbase_txid": template.coinbase_txid,
                    "template_utreexo_commitment": template.utreexo_commitment,
                    "merkle_root": merkle_root_hex,
                    "nonce": nonce,
                    "header_hash": self.hash_to_hex(header_hash),
                }
                return (block, nonce)

            # Progress report
            if nonce > 0 and nonce % report_interval == 0:
                elapsed = time.time() - start_time
                hashrate = nonce / max(elapsed, 0.001)
                print(f"  Progress: {nonce:,} hashes, {hashrate/1000:.2f} kH/s")

            nonce += 1

        print(f"[INFO] Nonce space exhausted without finding block")
        return None

    def build_full_block(self, header: bytes, coinbase_tx: bytes,
                        transactions: List[Dict[str, Any]]) -> bytes:
        """Build complete block for submission"""
        block = bytearray(header)

        # Transaction count
        tx_count = 1 + len(transactions)
        block.extend(self._encode_varint(tx_count))

        # Coinbase transaction
        block.extend(coinbase_tx)

        # Other transactions
        for tx in transactions:
            tx_data = bytes.fromhex(tx["data"])
            block.extend(tx_data)

        # Explicitly encode "no optional Utreexo payload".
        block.append(0x00)

        return bytes(block)

    def _encode_varint(self, n: int) -> bytes:
        """Encode integer as varint"""
        if n < 0xfd:
            return bytes([n])
        elif n <= 0xffff:
            return b'\xfd' + struct.pack('<H', n)
        elif n <= 0xffffffff:
            return b'\xfe' + struct.pack('<I', n)
        else:
            return b'\xff' + struct.pack('<Q', n)

    def submit_block(self, block_hex: str) -> bool:
        """Submit solved block to daemon"""
        try:
            result = self.rpc_call("submitblock", [block_hex])

            # Different RPC adapters may surface BIP22 success as null or {}.
            if result is None or result == {}:
                print(f"[SUCCESS] Block submitted and accepted!")
                self.blocks_found += 1
                return True

            # Any other response is an error
            print(f"[ERROR] Block rejected: {result}")
            return False

        except Exception as e:
            print(f"[ERROR] Submit failed: {e}")
            return False

    def get_mining_info(self) -> Dict[str, Any]:
        """Get current mining status"""
        try:
            return self.rpc_call("getmininginfo")
        except:
            return {}

    def get_block_count(self) -> int:
        """Get current block height"""
        try:
            return self.rpc_call("getblockcount")
        except:
            return -1

    def write_report(self, path: str, accepted: bool) -> None:
        """Persist the last mining attempt for integration tests."""
        payload = dict(self.last_mining_result)
        payload["accepted"] = accepted
        payload["blocks_found"] = self.blocks_found
        payload["total_hashes"] = self.total_hashes
        with open(path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")

    def run(self, num_blocks: int = 1, continuous: bool = False, report_path: Optional[str] = None):
        """Main mining loop"""
        print("=" * 60)
        print("DineroCoin CPU Miner")
        print("=" * 60)

        # Load authentication
        if not self.load_cookie_auth():
            print("[ERROR] Cannot proceed without RPC authentication")
            return False

        # Verify connection
        print(f"[INFO] Connecting to {self.rpc_url}...")
        try:
            height = self.get_block_count()
            print(f"[INFO] Connected! Current height: {height}")
        except Exception as e:
            print(f"[ERROR] Failed to connect: {e}")
            return False

        # Validate mining address
        if not self.mining_address:
            print("[ERROR] No mining address specified")
            return False
        print(f"[INFO] Mining to address: {self.mining_address}")

        blocks_mined = 0

        while blocks_mined < num_blocks or continuous:
            # Get new template
            template = self.get_block_template()
            if not template:
                print("[ERROR] Failed to get block template, retrying...")
                time.sleep(1)
                continue

            # Mine block
            result = self.mine_block(template)

            if result:
                block_bytes, nonce = result
                block_hex = block_bytes.hex()

                # Submit block
                accepted = self.submit_block(block_hex)
                if report_path:
                    self.write_report(report_path, accepted)
                if accepted:
                    blocks_mined += 1
                    print(f"[INFO] Blocks mined: {blocks_mined}/{num_blocks}")
            else:
                # Template may be stale, get new one
                print("[INFO] Getting fresh template...")
                continue

        # Final stats
        elapsed = time.time() - self.start_time
        print("\n" + "=" * 60)
        print("Mining Session Complete")
        print("=" * 60)
        print(f"  Total blocks found: {self.blocks_found}")
        print(f"  Total hashes: {self.total_hashes:,}")
        print(f"  Elapsed time: {elapsed:.2f}s")
        print(f"  Average hashrate: {self.total_hashes/max(elapsed,1)/1000:.2f} kH/s")

        return True


def main():
    parser = argparse.ArgumentParser(description="DineroCoin CPU Miner")
    parser.add_argument("--rpc-url", default="http://127.0.0.1:22020",
                       help="Daemon RPC URL (default: http://127.0.0.1:22020)")
    parser.add_argument("--cookie", default=None,
                       help="Path to .cookie file (auto-detected if not specified)")
    parser.add_argument("--address", required=True,
                       help="Mining payout address (required)")
    parser.add_argument("--blocks", type=int, default=1,
                       help="Number of blocks to mine (default: 1)")
    parser.add_argument("--continuous", action="store_true",
                       help="Mine continuously")
    parser.add_argument("--report", default=None,
                       help="Optional JSON report path for the mined template and block")

    args = parser.parse_args()

    miner = DineroCoinMiner(
        rpc_url=args.rpc_url,
        cookie_path=args.cookie,
        mining_address=args.address
    )

    success = miner.run(num_blocks=args.blocks, continuous=args.continuous, report_path=args.report)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
