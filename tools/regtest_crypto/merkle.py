"""
DineroCoin Merkle Tree Implementation

Reference implementation matching consensus/merkle.cpp exactly.
"""

from typing import List
from .pow import double_sha256

# =============================================================================
# Merkle Root Computation
# =============================================================================

def merkle_root(txids: List[bytes]) -> bytes:
    """
    Compute merkle root from transaction IDs.

    Matches consensus/merkle.cpp ComputeMerkleRoot() exactly.

    Args:
        txids: List of 32-byte txids (internal byte order, little-endian)

    Returns:
        32-byte merkle root (internal byte order)

    Algorithm:
        1. For single tx: root = txid
        2. For multiple: build binary tree, hash pairs
        3. If odd count: duplicate last element
    """
    if not txids:
        return b'\x00' * 32

    if len(txids) == 1:
        return txids[0]

    # Work on a copy
    layer = list(txids)

    while len(layer) > 1:
        # Duplicate last if odd
        if len(layer) % 2 == 1:
            layer.append(layer[-1])

        # Hash adjacent pairs
        next_layer = []
        for i in range(0, len(layer), 2):
            combined = layer[i] + layer[i + 1]
            next_layer.append(double_sha256(combined))

        layer = next_layer

    return layer[0]

def merkle_root_hex(txid_hexes: List[str], from_display: bool = True) -> str:
    """
    Compute merkle root from hex txid strings.

    Args:
        txid_hexes: List of 64-char hex txids
        from_display: If True, txids are in display format (big-endian)

    Returns:
        Merkle root as hex string (display format)
    """
    txids = []
    for txid_hex in txid_hexes:
        txid_bytes = bytes.fromhex(txid_hex)
        if from_display:
            txid_bytes = txid_bytes[::-1]  # Convert to internal order
        txids.append(txid_bytes)

    root = merkle_root(txids)
    return root[::-1].hex()  # Return in display format

# =============================================================================
# Merkle Proof
# =============================================================================

def merkle_path(txids: List[bytes], index: int) -> List[bytes]:
    """
    Compute merkle proof path for transaction at index.

    Args:
        txids: All transaction IDs
        index: Index of transaction to prove

    Returns:
        List of sibling hashes needed to verify inclusion
    """
    if not txids or index >= len(txids):
        return []

    if len(txids) == 1:
        return []

    path = []
    layer = list(txids)
    idx = index

    while len(layer) > 1:
        # Duplicate last if odd
        if len(layer) % 2 == 1:
            layer.append(layer[-1])

        # Get sibling
        if idx % 2 == 0:
            sibling = layer[idx + 1]
        else:
            sibling = layer[idx - 1]
        path.append(sibling)

        # Build next layer
        next_layer = []
        for i in range(0, len(layer), 2):
            combined = layer[i] + layer[i + 1]
            next_layer.append(double_sha256(combined))

        layer = next_layer
        idx //= 2

    return path

def verify_merkle_proof(txid: bytes, index: int, path: List[bytes], root: bytes) -> bool:
    """
    Verify merkle inclusion proof.

    Args:
        txid: Transaction ID to verify
        index: Position in original list
        path: Sibling hashes from merkle_path()
        root: Expected merkle root

    Returns:
        True if proof is valid
    """
    current = txid
    idx = index

    for sibling in path:
        if idx % 2 == 0:
            combined = current + sibling
        else:
            combined = sibling + current
        current = double_sha256(combined)
        idx //= 2

    return current == root

# =============================================================================
# Witness Commitment (BIP141)
# =============================================================================

def witness_commitment(wtxids: List[bytes], witness_reserved: bytes = b'\x00' * 32) -> bytes:
    """
    Compute witness commitment for segwit blocks.

    Matches BIP141 specification:
        commitment = SHA256(SHA256(witness_root || witness_reserved))

    Args:
        wtxids: Witness txids (wtxid of coinbase is 0x00...00)
        witness_reserved: Witness reserved value from coinbase (usually zeros)

    Returns:
        32-byte witness commitment
    """
    # Coinbase wtxid is always zeros
    wtxids_with_coinbase = [b'\x00' * 32] + wtxids[1:] if wtxids else []

    witness_root = merkle_root(wtxids_with_coinbase) if wtxids_with_coinbase else b'\x00' * 32

    return double_sha256(witness_root + witness_reserved)

def make_witness_commitment_script(commitment: bytes) -> bytes:
    """
    Create witness commitment output script.

    Format: OP_RETURN <commitment_header> <commitment>
    Header: 0xaa21a9ed (BIP141 magic)
    """
    header = bytes.fromhex("aa21a9ed")
    return bytes([0x6a, 0x24]) + header + commitment  # OP_RETURN PUSH36
