"""
DineroCoin Proof-of-Work Implementation

Reference implementation matching consensus/pow.cpp exactly.
Used for test vector generation and C++ verification.

Key differences from Bitcoin:
- 128-byte header (not 80)
- Field order: version, prev_block_hash, merkle_root, utreexo_root, timestamp, difficulty, nonce, reserved
- Timestamp is uint64 (8 bytes), not uint32 (4 bytes)
- Nonce at offset 112 (not 76)
- 12-byte reserved field (must be zeros)
"""

import hashlib
import struct
from dataclasses import dataclass, field
from typing import Optional, Tuple

from .params import DINERO_HEADER_SIZE, get_network

# =============================================================================
# Hashing
# =============================================================================

def sha256(data: bytes) -> bytes:
    """Single SHA-256."""
    return hashlib.sha256(data).digest()

def double_sha256(data: bytes) -> bytes:
    """Double SHA-256 (Bitcoin/DineroCoin standard)."""
    return sha256(sha256(data))

# =============================================================================
# Difficulty / Target Conversion
# =============================================================================

def bits_to_target(bits: int) -> int:
    """
    Convert compact "bits" to 256-bit target integer.

    Format: bits[31:24] = exponent, bits[23:0] = mantissa
    Formula: target = mantissa * 256^(exponent - 3)

    Matches consensus/pow.cpp BitsToTarget() exactly.
    """
    exponent = bits >> 24
    mantissa = bits & 0x00FFFFFF

    # Check for invalid values
    if exponent == 0:
        return 0
    if mantissa & 0x00800000:  # Negative (sign bit set)
        return 0

    if exponent <= 3:
        return mantissa >> (8 * (3 - exponent))
    else:
        return mantissa << (8 * (exponent - 3))

def target_to_bits(target: int) -> int:
    """Convert 256-bit target to compact bits representation."""
    if target == 0:
        return 0

    # Find byte length
    target_bytes = []
    t = target
    while t:
        target_bytes.append(t & 0xFF)
        t >>= 8
    target_bytes.reverse()

    size = len(target_bytes)

    # Extract mantissa (top 3 bytes)
    if size <= 3:
        mantissa = target << (8 * (3 - size))
    else:
        mantissa = target >> (8 * (size - 3))

    # Handle negative bit
    if mantissa & 0x00800000:
        mantissa >>= 8
        size += 1

    return (size << 24) | (mantissa & 0x00FFFFFF)

def bits_to_difficulty(bits: int) -> float:
    """Calculate difficulty relative to minimum (0x1d00ffff)."""
    min_target = bits_to_target(0x1d00ffff)
    current_target = bits_to_target(bits)
    if current_target == 0:
        return float('inf')
    return min_target / current_target

# =============================================================================
# Block Header (128 bytes for DineroCoin)
# =============================================================================

@dataclass
class DineroHeader:
    """
    DineroCoin 128-byte block header (BlockHeader v1 — FROZEN).

    Layout (little-endian, matches include/mining/header_layout.h):
        0-3:     version (4 bytes, LE uint32)
        4-35:    prev_block_hash (32 bytes, internal order)
        36-67:   merkle_root (32 bytes, internal order)
        68-99:   utreexo_root (32 bytes, internal order)
        100-107: timestamp (8 bytes, LE uint64)
        108-111: bits / difficulty (4 bytes, LE uint32)
        112-115: nonce (4 bytes, LE uint32)
        116-127: reserved (12 bytes, must be zeros)

    Total: 128 bytes (DINERO_HEADER_SIZE)
    """
    version: int = 1
    prev_block_hash: bytes = field(default_factory=lambda: b'\x00' * 32)
    merkle_root: bytes = field(default_factory=lambda: b'\x00' * 32)
    utreexo_root: bytes = field(default_factory=lambda: b'\x00' * 32)
    timestamp: int = 0
    bits: int = 0x207fffff
    nonce: int = 0
    reserved: bytes = field(default_factory=lambda: b'\x00' * 12)

    def serialize(self) -> bytes:
        """Serialize to 128 bytes for hashing."""
        assert len(self.prev_block_hash) == 32
        assert len(self.merkle_root) == 32
        assert len(self.utreexo_root) == 32
        assert len(self.reserved) == 12

        return (
            struct.pack('<I', self.version) +        # 4   offset 0
            self.prev_block_hash +                    # 32  offset 4
            self.merkle_root +                        # 32  offset 36
            self.utreexo_root +                       # 32  offset 68
            struct.pack('<Q', self.timestamp) +       # 8   offset 100
            struct.pack('<I', self.bits) +            # 4   offset 108
            struct.pack('<I', self.nonce) +           # 4   offset 112
            self.reserved                             # 12  offset 116
        )                                             # = 128

    @classmethod
    def deserialize(cls, data: bytes) -> 'DineroHeader':
        """Deserialize from 128 bytes."""
        if len(data) != DINERO_HEADER_SIZE:
            raise ValueError(f"Header must be {DINERO_HEADER_SIZE} bytes, got {len(data)}")

        return cls(
            version=struct.unpack('<I', data[0:4])[0],
            prev_block_hash=data[4:36],
            merkle_root=data[36:68],
            utreexo_root=data[68:100],
            timestamp=struct.unpack('<Q', data[100:108])[0],
            bits=struct.unpack('<I', data[108:112])[0],
            nonce=struct.unpack('<I', data[112:116])[0],
            reserved=data[116:128],
        )

    def get_hash(self) -> bytes:
        """Compute block hash (double SHA-256, internal byte order)."""
        return double_sha256(self.serialize())

    def get_hash_hex(self, display: bool = True) -> str:
        """
        Get hash as hex string.

        Args:
            display: If True, use display format (reversed, big-endian)
        """
        h = self.get_hash()
        if display:
            return h[::-1].hex()
        return h.hex()

# =============================================================================
# PoW Validation
# =============================================================================

def check_pow(header: DineroHeader, require_standard: bool = False) -> bool:
    """
    Check if header satisfies Proof-of-Work.

    Matches consensus/pow.cpp CheckProofOfWork() exactly.

    Args:
        header: Block header to validate
        require_standard: If True, validate difficulty bits range

    Returns:
        True if hash <= target
    """
    bits = header.bits

    # Validate difficulty bits if required (mainnet/testnet)
    if require_standard:
        if not check_difficulty_bits(bits):
            return False

    # Compute target and hash
    target = bits_to_target(bits)
    hash_bytes = header.get_hash()

    # Compare hash <= target (both as little-endian integers)
    hash_int = int.from_bytes(hash_bytes, 'little')

    return hash_int <= target

def check_difficulty_bits(bits: int) -> bool:
    """
    Validate difficulty bits are within allowed range.

    Matches consensus/pow.cpp CheckDifficultyBits().
    """
    exponent = bits >> 24
    mantissa = bits & 0x00FFFFFF

    # Exponent range: 1-32
    if exponent < 1 or exponent > 32:
        return False

    # No negative targets
    if mantissa & 0x00800000:
        return False

    # No zero targets
    if mantissa == 0 and exponent <= 3:
        return False

    # Target <= max_target (difficulty >= 1)
    target = bits_to_target(bits)
    max_target = bits_to_target(0x1d00ffff)

    return target <= max_target

# =============================================================================
# Mining
# =============================================================================

def mine_header(header: DineroHeader,
                max_nonce: int = 0xFFFFFFFF,
                start_nonce: int = 0,
                verbose: bool = False) -> Tuple[int, str]:
    """
    Find valid nonce for header.

    For regtest (0x207fffff): ~1-10 attempts
    For mainnet (0x1d00ffff): ~2^32 attempts (don't try!)

    Returns:
        (nonce, hash_hex) tuple

    Raises:
        RuntimeError if no valid nonce found
    """
    target = bits_to_target(header.bits)

    # Pre-compute static parts (nonce at offset 112)
    # prefix = version(4) + prev_block_hash(32) + merkle_root(32) +
    #          utreexo_root(32) + timestamp(8) + bits(4) = 112 bytes
    prefix = (
        struct.pack('<I', header.version) +
        header.prev_block_hash +
        header.merkle_root +
        header.utreexo_root +
        struct.pack('<Q', header.timestamp) +
        struct.pack('<I', header.bits)
    )
    suffix = header.reserved  # 12 bytes (offset 116)

    for nonce in range(start_nonce, max_nonce + 1):
        # Build full header with current nonce
        full = prefix + struct.pack('<I', nonce) + suffix
        hash_bytes = double_sha256(full)
        hash_int = int.from_bytes(hash_bytes, 'little')

        if hash_int <= target:
            if verbose:
                print(f"[MINE] Found nonce={nonce}, hash={hash_bytes[::-1].hex()[:16]}...")
            return nonce, hash_bytes[::-1].hex()

        if verbose and nonce > 0 and nonce % 1_000_000 == 0:
            print(f"[MINE] Tried {nonce:,} nonces...")

    raise RuntimeError(f"Failed to mine after {max_nonce - start_nonce + 1} attempts")

def mine_header_bytes(header_bytes: bytes,
                      bits: int = 0x207fffff,
                      max_nonce: int = 0xFFFFFFFF) -> Tuple[int, str]:
    """
    Mine raw header bytes (convenience wrapper).

    Args:
        header_bytes: 128-byte header with nonce at bytes 112-116
        bits: Difficulty bits (default: regtest)

    Returns:
        (nonce, hash_hex) tuple
    """
    if len(header_bytes) != DINERO_HEADER_SIZE:
        raise ValueError(f"Header must be {DINERO_HEADER_SIZE} bytes")

    header = DineroHeader.deserialize(header_bytes)
    header.bits = bits
    return mine_header(header, max_nonce=max_nonce)

# =============================================================================
# Chain Mining (multiple headers)
# =============================================================================

def mine_header_chain(count: int,
                      genesis_hash: bytes = None,
                      bits: int = 0x207fffff,
                      base_timestamp: int = 1_000_000,
                      verbose: bool = True) -> list:
    """
    Mine a chain of connected headers.

    Args:
        count: Number of headers to mine
        genesis_hash: Previous hash for first header (default: zeros)
        bits: Difficulty bits
        base_timestamp: Starting timestamp
        verbose: Print progress

    Returns:
        List of (header, nonce, hash_hex) tuples
    """
    if genesis_hash is None:
        genesis_hash = b'\x00' * 32

    results = []
    prev_hash = genesis_hash

    for i in range(count):
        header = DineroHeader(
            version=1,
            prev_block_hash=prev_hash,
            merkle_root=b'\x00' * 32,
            timestamp=base_timestamp + i,
            bits=bits,
            nonce=0,
            utreexo_root=b'\x00' * 32,
            reserved=b'\x00' * 12,
        )

        if verbose:
            print(f"Mining header {i+1}/{count}...", end=" ", flush=True)

        nonce, hash_hex = mine_header(header, verbose=False)
        header.nonce = nonce

        if verbose:
            print(f"nonce={nonce}, hash={hash_hex[:16]}...")

        results.append((header, nonce, hash_hex))
        prev_hash = header.get_hash()

    return results
