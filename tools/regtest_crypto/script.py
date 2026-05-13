"""
DineroCoin Script Operations

Minimal script building for P2PKH, P2WPKH, P2WSH, P2TR.
Matches Bitcoin Script semantics exactly.
"""

from typing import List, Optional
from .keys import hash160, tagged_hash

# =============================================================================
# Opcodes (subset needed for standard scripts)
# =============================================================================

# Constants
OP_0 = 0x00
OP_FALSE = OP_0
OP_PUSHDATA1 = 0x4c
OP_PUSHDATA2 = 0x4d
OP_PUSHDATA4 = 0x4e
OP_1NEGATE = 0x4f
OP_RESERVED = 0x50
OP_1 = 0x51
OP_TRUE = OP_1
OP_2 = 0x52
OP_3 = 0x53
OP_4 = 0x54
OP_5 = 0x55
OP_6 = 0x56
OP_7 = 0x57
OP_8 = 0x58
OP_9 = 0x59
OP_10 = 0x5a
OP_11 = 0x5b
OP_12 = 0x5c
OP_13 = 0x5d
OP_14 = 0x5e
OP_15 = 0x5f
OP_16 = 0x60

# Flow control
OP_NOP = 0x61
OP_IF = 0x63
OP_NOTIF = 0x64
OP_ELSE = 0x67
OP_ENDIF = 0x68
OP_VERIFY = 0x69
OP_RETURN = 0x6a

# Stack
OP_DUP = 0x76
OP_DROP = 0x75
OP_SWAP = 0x7c

# Crypto
OP_RIPEMD160 = 0xa6
OP_SHA256 = 0xa8
OP_HASH160 = 0xa9
OP_HASH256 = 0xaa
OP_CHECKSIG = 0xac
OP_CHECKSIGVERIFY = 0xad
OP_CHECKMULTISIG = 0xae
OP_CHECKMULTISIGVERIFY = 0xaf
OP_CHECKSIGADD = 0xba  # Tapscript

# Comparison
OP_EQUAL = 0x87
OP_EQUALVERIFY = 0x88

# =============================================================================
# Script Building Helpers
# =============================================================================

def push_data(data: bytes) -> bytes:
    """
    Create minimal push opcode for data.

    Follows BIP62 rules for minimal push.
    """
    length = len(data)

    if length == 0:
        return bytes([OP_0])
    elif length == 1 and data[0] <= 16:
        if data[0] == 0:
            return bytes([OP_0])
        else:
            return bytes([OP_1 + data[0] - 1])
    elif length == 1 and data[0] == 0x81:
        return bytes([OP_1NEGATE])
    elif length <= 75:
        return bytes([length]) + data
    elif length <= 255:
        return bytes([OP_PUSHDATA1, length]) + data
    elif length <= 65535:
        return bytes([OP_PUSHDATA2, length & 0xFF, length >> 8]) + data
    else:
        return bytes([OP_PUSHDATA4]) + length.to_bytes(4, 'little') + data

def push_int(n: int) -> bytes:
    """Push integer onto stack (minimal encoding)."""
    if n == 0:
        return bytes([OP_0])
    elif 1 <= n <= 16:
        return bytes([OP_1 + n - 1])
    elif n == -1:
        return bytes([OP_1NEGATE])
    else:
        # Encode as minimal byte array
        neg = n < 0
        absn = abs(n)
        result = []
        while absn:
            result.append(absn & 0xFF)
            absn >>= 8

        # Add sign bit if needed
        if result[-1] & 0x80:
            result.append(0x80 if neg else 0x00)
        elif neg:
            result[-1] |= 0x80

        return push_data(bytes(result))

# =============================================================================
# Standard Script Types
# =============================================================================

def p2pkh_script(pubkey_hash: bytes) -> bytes:
    """
    P2PKH (Pay-to-Public-Key-Hash) scriptPubKey.

    Format: OP_DUP OP_HASH160 <20-byte hash> OP_EQUALVERIFY OP_CHECKSIG
    """
    if len(pubkey_hash) != 20:
        raise ValueError("P2PKH requires 20-byte pubkey hash")

    return bytes([OP_DUP, OP_HASH160]) + push_data(pubkey_hash) + bytes([OP_EQUALVERIFY, OP_CHECKSIG])

def p2pkh_from_pubkey(pubkey: bytes) -> bytes:
    """Create P2PKH scriptPubKey from compressed public key."""
    return p2pkh_script(hash160(pubkey))

def p2sh_script(script_hash: bytes) -> bytes:
    """
    P2SH (Pay-to-Script-Hash) scriptPubKey.

    Format: OP_HASH160 <20-byte hash> OP_EQUAL
    """
    if len(script_hash) != 20:
        raise ValueError("P2SH requires 20-byte script hash")

    return bytes([OP_HASH160]) + push_data(script_hash) + bytes([OP_EQUAL])

def p2wpkh_script(pubkey_hash: bytes) -> bytes:
    """
    P2WPKH (Pay-to-Witness-Public-Key-Hash) scriptPubKey.

    Format: OP_0 <20-byte hash>
    Native SegWit v0.
    """
    if len(pubkey_hash) != 20:
        raise ValueError("P2WPKH requires 20-byte pubkey hash")

    return bytes([OP_0]) + push_data(pubkey_hash)

def p2wpkh_from_pubkey(pubkey: bytes) -> bytes:
    """Create P2WPKH scriptPubKey from compressed public key."""
    return p2wpkh_script(hash160(pubkey))

def p2wsh_script(script_hash: bytes) -> bytes:
    """
    P2WSH (Pay-to-Witness-Script-Hash) scriptPubKey.

    Format: OP_0 <32-byte hash>
    Native SegWit v0 for scripts.
    """
    if len(script_hash) != 32:
        raise ValueError("P2WSH requires 32-byte script hash")

    return bytes([OP_0]) + push_data(script_hash)

def p2tr_script(x_only_pubkey: bytes) -> bytes:
    """
    P2TR (Pay-to-Taproot) scriptPubKey.

    Format: OP_1 <32-byte x-only pubkey>
    SegWit v1 (Taproot).
    """
    if len(x_only_pubkey) != 32:
        raise ValueError("P2TR requires 32-byte x-only pubkey")

    return bytes([OP_1]) + push_data(x_only_pubkey)

def op_return_script(data: bytes) -> bytes:
    """
    OP_RETURN output (provably unspendable).

    Used for data embedding (witness commitment, etc.)
    """
    return bytes([OP_RETURN]) + push_data(data)

# =============================================================================
# Script Type Detection
# =============================================================================

def get_script_type(script: bytes) -> str:
    """Identify standard script type."""
    if len(script) == 25:
        if script[0:3] == bytes([OP_DUP, OP_HASH160, 0x14]) and \
           script[23:25] == bytes([OP_EQUALVERIFY, OP_CHECKSIG]):
            return "p2pkh"

    if len(script) == 23:
        if script[0:2] == bytes([OP_HASH160, 0x14]) and \
           script[22] == OP_EQUAL:
            return "p2sh"

    if len(script) == 22:
        if script[0:2] == bytes([OP_0, 0x14]):
            return "p2wpkh"

    if len(script) == 34:
        if script[0:2] == bytes([OP_0, 0x20]):
            return "p2wsh"
        if script[0:2] == bytes([OP_1, 0x20]):
            return "p2tr"

    if script and script[0] == OP_RETURN:
        return "op_return"

    return "unknown"

def extract_witness_program(script: bytes) -> Optional[tuple]:
    """
    Extract witness version and program from segwit scriptPubKey.

    Returns:
        (version, program) tuple or None if not segwit
    """
    if len(script) < 4:
        return None

    # Check for witness version (OP_0 through OP_16)
    version_byte = script[0]
    if version_byte == OP_0:
        version = 0
    elif OP_1 <= version_byte <= OP_16:
        version = version_byte - OP_1 + 1
    else:
        return None

    # Check program length byte
    program_len = script[1]
    if program_len < 2 or program_len > 40:
        return None

    # Verify total length
    if len(script) != 2 + program_len:
        return None

    program = script[2:]
    return (version, program)

# =============================================================================
# Taproot Helpers
# =============================================================================

def taproot_tweak_pubkey(internal_pubkey: bytes, merkle_root: bytes = None) -> bytes:
    """
    Compute tweaked Taproot public key.

    Args:
        internal_pubkey: 32-byte x-only pubkey
        merkle_root: Optional 32-byte tapscript merkle root

    Returns:
        32-byte tweaked x-only pubkey
    """
    if merkle_root is None:
        merkle_root = b''

    t = tagged_hash("TapTweak", internal_pubkey + merkle_root)
    # Note: Full implementation requires EC point operations
    # This is a placeholder - use libsecp256k1 for production
    raise NotImplementedError("Taproot tweaking requires EC operations - use libsecp256k1")

def taproot_leaf_hash(script: bytes, leaf_version: int = 0xc0) -> bytes:
    """
    Compute Taproot leaf hash for tapscript.

    Format: tagged_hash("TapLeaf", leaf_version || compact_size(script) || script)
    """
    # Compact size encoding
    script_len = len(script)
    if script_len < 0xfd:
        size_bytes = bytes([script_len])
    elif script_len <= 0xffff:
        size_bytes = bytes([0xfd]) + script_len.to_bytes(2, 'little')
    else:
        size_bytes = bytes([0xfe]) + script_len.to_bytes(4, 'little')

    return tagged_hash("TapLeaf", bytes([leaf_version]) + size_bytes + script)

# =============================================================================
# Multisig Helpers
# =============================================================================

def multisig_script(m: int, pubkeys: List[bytes]) -> bytes:
    """
    Create m-of-n multisig script.

    Format: OP_m <pubkey1> ... <pubkeyn> OP_n OP_CHECKMULTISIG
    """
    n = len(pubkeys)

    if m < 1 or m > n or n > 16:
        raise ValueError(f"Invalid multisig parameters: {m}-of-{n}")

    script = push_int(m)
    for pubkey in pubkeys:
        script += push_data(pubkey)
    script += push_int(n)
    script += bytes([OP_CHECKMULTISIG])

    return script
