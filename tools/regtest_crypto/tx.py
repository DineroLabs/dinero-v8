"""
DineroCoin Transaction Building and Serialization

Matches wallet/transaction.h and primitives/transaction.h exactly.
Supports both legacy and SegWit (including Taproot) transactions.
"""

import struct
from dataclasses import dataclass, field
from typing import List, Optional
from enum import IntEnum

from .pow import double_sha256
from .keys import tagged_hash

# =============================================================================
# Constants
# =============================================================================

# Sighash types
class SigHashType(IntEnum):
    ALL = 0x01
    NONE = 0x02
    SINGLE = 0x03
    ANYONECANPAY = 0x80

# Sequence constants
SEQUENCE_FINAL = 0xFFFFFFFF
SEQUENCE_RBF = 0xFFFFFFFD  # RBF enabled

# Transaction version
TX_VERSION = 2

# Witness marker and flag
WITNESS_MARKER = 0x00
WITNESS_FLAG = 0x01

# =============================================================================
# Compact Size Encoding (varint)
# =============================================================================

def compact_size(n: int) -> bytes:
    """Encode integer as Bitcoin compact size."""
    if n < 0xfd:
        return bytes([n])
    elif n <= 0xffff:
        return bytes([0xfd]) + struct.pack('<H', n)
    elif n <= 0xffffffff:
        return bytes([0xfe]) + struct.pack('<I', n)
    else:
        return bytes([0xff]) + struct.pack('<Q', n)

def read_compact_size(data: bytes, offset: int = 0) -> tuple:
    """Read compact size from bytes, return (value, new_offset)."""
    first = data[offset]
    if first < 0xfd:
        return first, offset + 1
    elif first == 0xfd:
        return struct.unpack('<H', data[offset+1:offset+3])[0], offset + 3
    elif first == 0xfe:
        return struct.unpack('<I', data[offset+1:offset+5])[0], offset + 5
    else:
        return struct.unpack('<Q', data[offset+1:offset+9])[0], offset + 9

# =============================================================================
# Transaction Components
# =============================================================================

@dataclass
class OutPoint:
    """Transaction output reference (txid:vout)."""
    txid: bytes = field(default_factory=lambda: b'\x00' * 32)  # 32 bytes, internal order
    vout: int = 0

    def serialize(self) -> bytes:
        return self.txid + struct.pack('<I', self.vout)

    @classmethod
    def deserialize(cls, data: bytes, offset: int = 0) -> tuple:
        txid = data[offset:offset+32]
        vout = struct.unpack('<I', data[offset+32:offset+36])[0]
        return cls(txid=txid, vout=vout), offset + 36

    def is_null(self) -> bool:
        """Check if this is a coinbase outpoint."""
        return self.txid == b'\x00' * 32 and self.vout == 0xFFFFFFFF

@dataclass
class TxInput:
    """Transaction input."""
    prevout: OutPoint = field(default_factory=OutPoint)
    script_sig: bytes = b''
    sequence: int = SEQUENCE_FINAL
    witness: List[bytes] = field(default_factory=list)

    def serialize(self, include_witness: bool = False) -> bytes:
        """Serialize input (witness handled separately in tx serialization)."""
        result = self.prevout.serialize()
        result += compact_size(len(self.script_sig))
        result += self.script_sig
        result += struct.pack('<I', self.sequence)
        return result

    def serialize_witness(self) -> bytes:
        """Serialize witness stack."""
        result = compact_size(len(self.witness))
        for item in self.witness:
            result += compact_size(len(item))
            result += item
        return result

@dataclass
class TxOutput:
    """Transaction output."""
    value: int = 0  # In una (una)
    script_pubkey: bytes = b''

    def serialize(self) -> bytes:
        result = struct.pack('<Q', self.value)
        result += compact_size(len(self.script_pubkey))
        result += self.script_pubkey
        return result

    @classmethod
    def deserialize(cls, data: bytes, offset: int = 0) -> tuple:
        value = struct.unpack('<Q', data[offset:offset+8])[0]
        script_len, new_offset = read_compact_size(data, offset + 8)
        script_pubkey = data[new_offset:new_offset+script_len]
        return cls(value=value, script_pubkey=script_pubkey), new_offset + script_len

# =============================================================================
# Transaction
# =============================================================================

@dataclass
class Transaction:
    """
    DineroCoin transaction.

    Supports both legacy and SegWit serialization formats.
    """
    version: int = TX_VERSION
    inputs: List[TxInput] = field(default_factory=list)
    outputs: List[TxOutput] = field(default_factory=list)
    locktime: int = 0

    def has_witness(self) -> bool:
        """Check if any input has witness data."""
        return any(inp.witness for inp in self.inputs)

    def serialize(self, include_witness: bool = True) -> bytes:
        """
        Serialize transaction.

        Args:
            include_witness: Include witness data (for txid, set False)
        """
        result = struct.pack('<I', self.version)

        use_witness = include_witness and self.has_witness()

        if use_witness:
            result += bytes([WITNESS_MARKER, WITNESS_FLAG])

        # Inputs
        result += compact_size(len(self.inputs))
        for inp in self.inputs:
            result += inp.serialize()

        # Outputs
        result += compact_size(len(self.outputs))
        for out in self.outputs:
            result += out.serialize()

        # Witness data
        if use_witness:
            for inp in self.inputs:
                result += inp.serialize_witness()

        result += struct.pack('<I', self.locktime)
        return result

    def get_txid(self) -> bytes:
        """
        Compute transaction ID (without witness).

        Returns 32 bytes in internal order (little-endian).
        """
        return double_sha256(self.serialize(include_witness=False))

    def get_txid_hex(self, display: bool = True) -> str:
        """Get txid as hex string."""
        txid = self.get_txid()
        if display:
            return txid[::-1].hex()
        return txid.hex()

    def get_wtxid(self) -> bytes:
        """
        Compute witness transaction ID (with witness).

        For non-witness transactions, wtxid == txid.
        """
        return double_sha256(self.serialize(include_witness=True))

    def get_wtxid_hex(self, display: bool = True) -> str:
        """Get wtxid as hex string."""
        wtxid = self.get_wtxid()
        if display:
            return wtxid[::-1].hex()
        return wtxid.hex()

    def get_virtual_size(self) -> int:
        """
        Calculate virtual size (vbytes) for fee calculation.

        vsize = (weight + 3) / 4
        weight = base_size * 3 + total_size
        """
        base_size = len(self.serialize(include_witness=False))
        total_size = len(self.serialize(include_witness=True))
        weight = base_size * 3 + total_size
        return (weight + 3) // 4

# =============================================================================
# Sighash Computation
# =============================================================================

def sighash_legacy(tx: Transaction, input_index: int, script_code: bytes,
                   sighash_type: int = SigHashType.ALL) -> bytes:
    """
    Compute legacy sighash for signing.

    This is the pre-SegWit sighash algorithm.
    """
    # Create modified transaction copy
    modified_tx = Transaction(
        version=tx.version,
        inputs=[],
        outputs=list(tx.outputs),
        locktime=tx.locktime,
    )

    # Clear all input scripts, set script_code for signing input
    for i, inp in enumerate(tx.inputs):
        new_inp = TxInput(
            prevout=inp.prevout,
            script_sig=script_code if i == input_index else b'',
            sequence=inp.sequence,
        )
        modified_tx.inputs.append(new_inp)

    # Handle ANYONECANPAY
    if sighash_type & SigHashType.ANYONECANPAY:
        modified_tx.inputs = [modified_tx.inputs[input_index]]
        input_index = 0

    # Handle NONE
    base_type = sighash_type & 0x1f
    if base_type == SigHashType.NONE:
        modified_tx.outputs = []
        for i, inp in enumerate(modified_tx.inputs):
            if i != input_index:
                inp.sequence = 0

    # Handle SINGLE
    elif base_type == SigHashType.SINGLE:
        if input_index >= len(modified_tx.outputs):
            # Bitcoin bug: return 1 as hash
            return b'\x01' + b'\x00' * 31
        modified_tx.outputs = modified_tx.outputs[:input_index + 1]
        for i in range(input_index):
            modified_tx.outputs[i] = TxOutput(value=0xffffffffffffffff, script_pubkey=b'')
        for i, inp in enumerate(modified_tx.inputs):
            if i != input_index:
                inp.sequence = 0

    # Serialize and hash with sighash type
    preimage = modified_tx.serialize(include_witness=False)
    preimage += struct.pack('<I', sighash_type)

    return double_sha256(preimage)

def sighash_segwit_v0(tx: Transaction, input_index: int, script_code: bytes,
                      value: int, sighash_type: int = SigHashType.ALL) -> bytes:
    """
    Compute BIP143 sighash for SegWit v0 (P2WPKH, P2WSH).
    """
    base_type = sighash_type & 0x1f
    anyone_can_pay = bool(sighash_type & SigHashType.ANYONECANPAY)

    # hashPrevouts
    if anyone_can_pay:
        hash_prevouts = b'\x00' * 32
    else:
        prevouts = b''.join(inp.prevout.serialize() for inp in tx.inputs)
        hash_prevouts = double_sha256(prevouts)

    # hashSequence
    if anyone_can_pay or base_type in (SigHashType.SINGLE, SigHashType.NONE):
        hash_sequence = b'\x00' * 32
    else:
        sequences = b''.join(struct.pack('<I', inp.sequence) for inp in tx.inputs)
        hash_sequence = double_sha256(sequences)

    # hashOutputs
    if base_type == SigHashType.NONE:
        hash_outputs = b'\x00' * 32
    elif base_type == SigHashType.SINGLE:
        if input_index < len(tx.outputs):
            hash_outputs = double_sha256(tx.outputs[input_index].serialize())
        else:
            hash_outputs = b'\x00' * 32
    else:
        outputs = b''.join(out.serialize() for out in tx.outputs)
        hash_outputs = double_sha256(outputs)

    # Build preimage (BIP143)
    inp = tx.inputs[input_index]
    preimage = (
        struct.pack('<I', tx.version) +
        hash_prevouts +
        hash_sequence +
        inp.prevout.serialize() +
        compact_size(len(script_code)) + script_code +
        struct.pack('<Q', value) +
        struct.pack('<I', inp.sequence) +
        hash_outputs +
        struct.pack('<I', tx.locktime) +
        struct.pack('<I', sighash_type)
    )

    return double_sha256(preimage)

def sighash_taproot(tx: Transaction, input_index: int, prevouts: List[TxOutput],
                    sighash_type: int = 0x00, ext_flag: int = 0,
                    annex: bytes = None, script: bytes = None,
                    leaf_version: int = None) -> bytes:
    """
    Compute BIP341 sighash for Taproot (P2TR).

    Args:
        tx: Transaction being signed
        input_index: Index of input being signed
        prevouts: All previous outputs being spent (for amounts/scripts)
        sighash_type: Sighash type (default 0x00 = SIGHASH_DEFAULT = ALL)
        ext_flag: Extension flag (0 = key path, 1 = script path)
        annex: Optional annex data
        script: Tapscript being executed (for script path)
        leaf_version: Leaf version (for script path)
    """
    if sighash_type == 0x00:
        sighash_type = SigHashType.ALL

    base_type = sighash_type & 0x1f
    anyone_can_pay = bool(sighash_type & SigHashType.ANYONECANPAY)

    # Epoch (always 0)
    preimage = bytes([0x00])

    # Control byte
    preimage += bytes([sighash_type])

    # Transaction data
    preimage += struct.pack('<I', tx.version)
    preimage += struct.pack('<I', tx.locktime)

    # Input data (if not ANYONECANPAY)
    if not anyone_can_pay:
        # sha_prevouts
        prevouts_data = b''.join(inp.prevout.serialize() for inp in tx.inputs)
        preimage += double_sha256(prevouts_data)

        # sha_amounts
        amounts = b''.join(struct.pack('<Q', p.value) for p in prevouts)
        preimage += double_sha256(amounts)

        # sha_scriptpubkeys
        scripts = b''.join(compact_size(len(p.script_pubkey)) + p.script_pubkey for p in prevouts)
        preimage += double_sha256(scripts)

        # sha_sequences
        sequences = b''.join(struct.pack('<I', inp.sequence) for inp in tx.inputs)
        preimage += double_sha256(sequences)

    # Output data (if ALL or DEFAULT)
    if base_type == SigHashType.ALL:
        outputs = b''.join(out.serialize() for out in tx.outputs)
        preimage += double_sha256(outputs)

    # Spend type
    spend_type = ext_flag * 2 + (1 if annex else 0)
    preimage += bytes([spend_type])

    # Input-specific data
    if anyone_can_pay:
        inp = tx.inputs[input_index]
        preimage += inp.prevout.serialize()
        preimage += struct.pack('<Q', prevouts[input_index].value)
        preimage += compact_size(len(prevouts[input_index].script_pubkey))
        preimage += prevouts[input_index].script_pubkey
        preimage += struct.pack('<I', inp.sequence)
    else:
        preimage += struct.pack('<I', input_index)

    # Annex hash
    if annex:
        preimage += double_sha256(compact_size(len(annex)) + annex)

    # Single output (if SINGLE)
    if base_type == SigHashType.SINGLE:
        if input_index < len(tx.outputs):
            preimage += double_sha256(tx.outputs[input_index].serialize())

    # Script path extension
    if ext_flag == 1 and script is not None:
        # tapleaf_hash
        leaf_data = bytes([leaf_version]) + compact_size(len(script)) + script
        tapleaf_hash = tagged_hash("TapLeaf", leaf_data)
        preimage += tapleaf_hash
        preimage += bytes([0x00])  # key_version
        preimage += struct.pack('<I', 0xffffffff)  # codesep_pos (none)

    return tagged_hash("TapSighash", preimage)

# =============================================================================
# Transaction Builder Helpers
# =============================================================================

def create_coinbase_tx(height: int, value: int, script_pubkey: bytes,
                       extra_data: bytes = b'') -> Transaction:
    """
    Create coinbase transaction for block at given height.

    Args:
        height: Block height (for BIP34 encoding)
        value: Block reward in una
        script_pubkey: Output scriptPubKey
        extra_data: Optional extra coinbase data (miner tag, etc.)
    """
    # BIP34 height encoding
    if height == 0:
        height_script = bytes([0x00])
    elif height <= 16:
        height_script = bytes([0x50 + height])  # OP_1 to OP_16
    else:
        # Minimal encoding
        height_bytes = height.to_bytes((height.bit_length() + 7) // 8, 'little')
        if height_bytes[-1] & 0x80:
            height_bytes += b'\x00'
        height_script = bytes([len(height_bytes)]) + height_bytes

    coinbase_input = TxInput(
        prevout=OutPoint(txid=b'\x00' * 32, vout=0xFFFFFFFF),
        script_sig=height_script + extra_data,
        sequence=SEQUENCE_FINAL,
    )

    coinbase_output = TxOutput(
        value=value,
        script_pubkey=script_pubkey,
    )

    return Transaction(
        version=TX_VERSION,
        inputs=[coinbase_input],
        outputs=[coinbase_output],
        locktime=0,
    )
