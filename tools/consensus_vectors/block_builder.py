"""
DineroCoin Block Builder

Builds valid blocks and intentionally-invalid blocks for consensus testing.
Each invalid block violates exactly one consensus rule.
"""

import struct
import time
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Dict, Any, Tuple
from pathlib import Path

# Add parent to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from regtest_crypto.pow import DineroHeader as PowHeader, double_sha256, mine_header, bits_to_target
from regtest_crypto.merkle import merkle_root
from regtest_crypto.tx import Transaction, TxInput, TxOutput, OutPoint, create_coinbase_tx, compact_size
from regtest_crypto.params import get_network

from .rules import ConsensusRules, ValidationError


@dataclass
class BlockHeader:
    """
    Block header structure (BlockHeader v1 — 128 bytes).

    Layout (matches include/mining/header_layout.h):
        0-3:     version (4 bytes, LE uint32)
        4-35:    prev_block_hash (32 bytes)
        36-67:   merkle_root (32 bytes)
        68-99:   utreexo_root (32 bytes)
        100-107: timestamp (8 bytes, LE uint64)
        108-111: bits (4 bytes, LE uint32)
        112-115: nonce (4 bytes, LE uint32)
        116-127: reserved (12 bytes, must be zeros)
    """
    version: int = 0x20000000
    prev_block: bytes = field(default_factory=lambda: b'\x00' * 32)
    merkle_root: bytes = field(default_factory=lambda: b'\x00' * 32)
    utreexo_root: bytes = field(default_factory=lambda: b'\x00' * 32)
    timestamp: int = 0
    bits: int = 0x207fffff
    nonce: int = 0
    reserved: bytes = field(default_factory=lambda: b'\x00' * 12)

    def serialize(self) -> bytes:
        """Serialize header to 128 bytes."""
        return (
            struct.pack('<I', self.version) +        # 4   offset 0
            self.prev_block +                         # 32  offset 4
            self.merkle_root +                        # 32  offset 36
            self.utreexo_root +                       # 32  offset 68
            struct.pack('<Q', self.timestamp) +       # 8   offset 100
            struct.pack('<I', self.bits) +            # 4   offset 108
            struct.pack('<I', self.nonce) +           # 4   offset 112
            self.reserved                             # 12  offset 116
        )                                             # = 128

    def to_pow_header(self) -> 'PowHeader':
        """Convert to regtest_crypto DineroHeader for mining."""
        return PowHeader(
            version=self.version,
            prev_block_hash=self.prev_block,
            merkle_root=self.merkle_root,
            utreexo_root=self.utreexo_root,
            timestamp=self.timestamp,
            bits=self.bits,
            nonce=self.nonce,
        )

    def hash(self) -> bytes:
        """Get block hash (double SHA256 of 128-byte header)."""
        return double_sha256(self.serialize())

    def hash_hex(self) -> str:
        """Get block hash in display order (big-endian)."""
        return self.hash()[::-1].hex()


@dataclass
class Block:
    """Full block structure"""
    header: BlockHeader
    transactions: List[Transaction] = field(default_factory=list)

    def serialize(self) -> bytes:
        """Serialize full block"""
        result = self.header.serialize()
        result += compact_size(len(self.transactions))
        for tx in self.transactions:
            result += tx.serialize()
        return result

    def hash(self) -> bytes:
        return self.header.hash()

    def hash_hex(self) -> str:
        return self.header.hash_hex()


class BlockBuilder:
    """
    Builds valid blocks for testing.

    Usage:
        builder = BlockBuilder(network="regtest")
        block = builder.create_block(
            prev_hash=genesis_hash,
            height=1,
            coinbase_address=my_address,
        )
    """

    def __init__(self, network: str = "regtest"):
        self.params = get_network(network)
        self.network = network

    def create_coinbase(
        self,
        height: int,
        address: bytes,
        subsidy: int,
        fees: int = 0,
        extra_data: bytes = b"",
    ) -> Transaction:
        """Create a coinbase transaction"""
        return create_coinbase_tx(
            height=height,
            address=address,
            subsidy=subsidy + fees,
            extra_data=extra_data,
        )

    def compute_merkle_root(self, transactions: List[Transaction]) -> bytes:
        """Compute merkle root of transactions"""
        txids = [tx.txid() for tx in transactions]
        return merkle_root(txids)

    def create_block(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
        transactions: Optional[List[Transaction]] = None,
        timestamp: Optional[int] = None,
        version: int = 0x20000000,
        extra_coinbase_data: bytes = b"",
        mine: bool = True,
    ) -> Block:
        """
        Create a valid block.

        Args:
            prev_hash: Previous block hash (32 bytes, internal order)
            height: Block height
            coinbase_address: Address for coinbase output
            transactions: Additional transactions (after coinbase)
            timestamp: Block timestamp (default: current time)
            version: Block version
            extra_coinbase_data: Extra data for coinbase scriptSig
            mine: If True, mine the block (find valid nonce)

        Returns:
            Complete valid block
        """
        # Calculate subsidy
        halvings = height // self.params.subsidy_halving_interval
        subsidy = 50 * 100_000_000  # 50 DIN in una
        subsidy >>= halvings
        if subsidy < 0:
            subsidy = 0

        # Calculate fees from transactions
        fees = 0  # TODO: calculate from inputs - outputs

        # Create coinbase
        coinbase = self.create_coinbase(
            height=height,
            address=coinbase_address,
            subsidy=subsidy,
            fees=fees,
            extra_data=extra_coinbase_data,
        )

        # Build transaction list
        txs = [coinbase]
        if transactions:
            txs.extend(transactions)

        # Compute merkle root
        merkle = self.compute_merkle_root(txs)

        # Build header
        header = BlockHeader(
            version=version,
            prev_block=prev_hash,
            merkle_root=merkle,
            timestamp=timestamp or int(time.time()),
            bits=self.params.bits,
            nonce=0,
        )

        # Mine if requested
        if mine:
            nonce, _ = mine_header(header.to_pow_header())
            header.nonce = nonce

        return Block(header=header, transactions=txs)

    def to_vector(self, block: Block, label: str = "") -> Dict[str, Any]:
        """Convert block to test vector format"""
        return {
            "label": label,
            "hash": block.hash_hex(),
            "height": None,  # Caller should set
            "header": {
                "version": block.header.version,
                "prev_block": block.header.prev_block[::-1].hex(),
                "merkle_root": block.header.merkle_root[::-1].hex(),
                "utreexo_root": block.header.utreexo_root[::-1].hex(),
                "timestamp": block.header.timestamp,
                "bits": f"0x{block.header.bits:08x}",
                "nonce": block.header.nonce,
            },
            "header_hex": block.header.serialize().hex(),
            "tx_count": len(block.transactions),
            "block_hex": block.serialize().hex(),
        }


class InvalidBlockBuilder(BlockBuilder):
    """
    Builds intentionally-invalid blocks for negative testing.

    Each method creates a block that violates exactly one consensus rule.
    """

    def __init__(self, network: str = "regtest"):
        super().__init__(network)

    def bad_pow(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with invalid PoW (hash > target).

        Violates: POW_VALID
        """
        block = self.create_block(
            prev_hash=prev_hash,
            height=height,
            coinbase_address=coinbase_address,
            mine=False,  # Don't mine - leave invalid nonce
        )
        # Set a nonce that definitely doesn't meet target
        block.header.nonce = 0

        return block, "POW_VALID"

    def bad_merkle_root(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with wrong merkle root.

        Violates: MERKLE_ROOT
        """
        block = self.create_block(
            prev_hash=prev_hash,
            height=height,
            coinbase_address=coinbase_address,
            mine=False,
        )
        # Corrupt merkle root
        block.header.merkle_root = b'\xde\xad\xbe\xef' + b'\x00' * 28

        # Re-mine with wrong merkle root
        nonce, _ = mine_header(block.header.to_pow_header())
        block.header.nonce = nonce

        return block, "MERKLE_ROOT"

    def bad_prev_hash(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with non-existent prev_block_hash.

        Violates: PREV_BLOCK
        """
        # Use a fake prev hash
        fake_prev = double_sha256(b"this block does not exist")

        block = self.create_block(
            prev_hash=fake_prev,
            height=height,
            coinbase_address=coinbase_address,
            mine=True,
        )

        return block, "PREV_BLOCK"

    def bad_timestamp_future(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with timestamp too far in future (>2 hours).

        Violates: TIMESTAMP_FUTURE
        """
        future_time = int(time.time()) + (3 * 60 * 60)  # 3 hours ahead

        block = self.create_block(
            prev_hash=prev_hash,
            height=height,
            coinbase_address=coinbase_address,
            timestamp=future_time,
            mine=True,
        )

        return block, "TIMESTAMP_FUTURE"

    def bad_coinbase_subsidy(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with coinbase claiming too much reward.

        Violates: COINBASE_SUBSIDY
        """
        # Create block normally first
        block = self.create_block(
            prev_hash=prev_hash,
            height=height,
            coinbase_address=coinbase_address,
            mine=False,
        )

        # Modify coinbase to claim extra
        if block.transactions:
            cb = block.transactions[0]
            if cb.outputs:
                cb.outputs[0].value += 100_000_000  # Add 1 extra DIN

        # Recompute merkle root and mine
        block.header.merkle_root = self.compute_merkle_root(block.transactions)
        nonce, _ = mine_header(block.header.to_pow_header())
        block.header.nonce = nonce

        return block, "COINBASE_SUBSIDY"

    def bad_no_coinbase(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> Tuple[Block, str]:
        """
        Create block with no transactions.

        Violates: TX_COUNT
        """
        header = BlockHeader(
            version=0x20000000,
            prev_block=prev_hash,
            merkle_root=b'\x00' * 32,
            timestamp=int(time.time()),
            bits=self.params.bits,
            nonce=0,
        )

        nonce, _ = mine_header(header.to_pow_header())
        header.nonce = nonce

        return Block(header=header, transactions=[]), "TX_COUNT"

    def bad_duplicate_tx(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
        tx_to_duplicate: Transaction,
    ) -> Tuple[Block, str]:
        """
        Create block with duplicate transaction.

        Violates: NO_DUP_INPUTS (same tx included twice)
        """
        block = self.create_block(
            prev_hash=prev_hash,
            height=height,
            coinbase_address=coinbase_address,
            transactions=[tx_to_duplicate, tx_to_duplicate],  # Duplicate!
            mine=True,
        )

        return block, "NO_DUP_INPUTS"

    def generate_all_invalid(
        self,
        prev_hash: bytes,
        height: int,
        coinbase_address: bytes,
    ) -> List[Tuple[Block, str, str]]:
        """
        Generate all types of invalid blocks.

        Returns: List of (block, rule_violated, description)
        """
        results = []

        # Each invalid block type
        generators = [
            ("bad_pow", "Block with invalid proof of work"),
            ("bad_merkle_root", "Block with wrong merkle root"),
            ("bad_prev_hash", "Block with non-existent prev hash"),
            ("bad_timestamp_future", "Block with timestamp >2h in future"),
            ("bad_coinbase_subsidy", "Block claiming too much subsidy"),
            ("bad_no_coinbase", "Block with no transactions"),
        ]

        for method_name, description in generators:
            method = getattr(self, method_name)
            try:
                block, rule = method(prev_hash, height, coinbase_address)
                results.append((block, rule, description))
            except Exception as e:
                print(f"Warning: {method_name} failed: {e}")

        return results
