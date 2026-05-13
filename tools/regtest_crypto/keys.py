"""
DineroCoin Key and Signature Operations

ECDSA (secp256k1) for legacy transactions.
Schnorr (BIP340) for Taproot (P2TR).

WARNING: For test vector generation only!
Never use for real funds - use hardware wallets.
"""

import hashlib
import hmac
import secrets
from typing import Tuple, Optional
from dataclasses import dataclass

# =============================================================================
# secp256k1 Parameters
# =============================================================================

# Curve order
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141

# Generator point (compressed)
G_X = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
G_Y = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

# Field prime
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F

# =============================================================================
# Elliptic Curve Operations (minimal, for test vectors)
# =============================================================================

def modinv(a: int, m: int) -> int:
    """Modular inverse using extended Euclidean algorithm."""
    if a < 0:
        a = a % m
    g, x, _ = extended_gcd(a, m)
    if g != 1:
        raise ValueError("No modular inverse")
    return x % m

def extended_gcd(a: int, b: int) -> Tuple[int, int, int]:
    """Extended Euclidean algorithm."""
    if a == 0:
        return b, 0, 1
    g, x, y = extended_gcd(b % a, a)
    return g, y - (b // a) * x, x

def point_add(p1: Optional[Tuple[int, int]], p2: Optional[Tuple[int, int]]) -> Optional[Tuple[int, int]]:
    """Add two points on secp256k1."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1

    x1, y1 = p1
    x2, y2 = p2

    if x1 == x2:
        if y1 != y2:
            return None  # Point at infinity
        # Point doubling
        s = (3 * x1 * x1 * modinv(2 * y1, P)) % P
    else:
        s = ((y2 - y1) * modinv(x2 - x1, P)) % P

    x3 = (s * s - x1 - x2) % P
    y3 = (s * (x1 - x3) - y1) % P

    return (x3, y3)

def point_mul(k: int, point: Tuple[int, int] = None) -> Optional[Tuple[int, int]]:
    """Scalar multiplication on secp256k1."""
    if point is None:
        point = (G_X, G_Y)

    result = None
    addend = point

    while k:
        if k & 1:
            result = point_add(result, addend)
        addend = point_add(addend, addend)
        k >>= 1

    return result

# =============================================================================
# Key Generation
# =============================================================================

@dataclass
class KeyPair:
    """secp256k1 key pair."""
    private_key: bytes      # 32 bytes
    public_key: bytes       # 33 bytes (compressed) or 65 bytes (uncompressed)
    x_only_pubkey: bytes    # 32 bytes (for Taproot/Schnorr)

def generate_keypair() -> KeyPair:
    """Generate random secp256k1 key pair."""
    # Generate random 32-byte private key
    while True:
        privkey = secrets.token_bytes(32)
        privkey_int = int.from_bytes(privkey, 'big')
        if 0 < privkey_int < N:
            break

    return keypair_from_privkey(privkey)

def keypair_from_privkey(privkey: bytes) -> KeyPair:
    """Derive public key from private key."""
    privkey_int = int.from_bytes(privkey, 'big')

    if privkey_int == 0 or privkey_int >= N:
        raise ValueError("Invalid private key")

    # Compute public key point
    point = point_mul(privkey_int)
    if point is None:
        raise ValueError("Invalid private key")

    x, y = point

    # Compressed public key (33 bytes)
    prefix = 0x02 if y % 2 == 0 else 0x03
    pubkey_compressed = bytes([prefix]) + x.to_bytes(32, 'big')

    # X-only pubkey for Taproot (32 bytes)
    x_only = x.to_bytes(32, 'big')

    return KeyPair(
        private_key=privkey,
        public_key=pubkey_compressed,
        x_only_pubkey=x_only,
    )

def privkey_from_wif(wif: str) -> bytes:
    """Decode WIF (Wallet Import Format) to raw private key."""
    # Base58 decode
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    num = 0
    for char in wif:
        num = num * 58 + alphabet.index(char)

    # Convert to bytes (typically 37 or 38 bytes with checksum)
    data = num.to_bytes((num.bit_length() + 7) // 8, 'big')

    # Verify checksum (last 4 bytes)
    checksum = data[-4:]
    payload = data[:-4]
    expected = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]

    if checksum != expected:
        raise ValueError("Invalid WIF checksum")

    # Extract private key (skip version byte, possibly compression flag)
    if len(payload) == 34:  # Compressed
        return payload[1:33]
    elif len(payload) == 33:  # Uncompressed
        return payload[1:33]
    else:
        raise ValueError(f"Unexpected WIF payload length: {len(payload)}")

# =============================================================================
# Hashing for Signatures
# =============================================================================

def tagged_hash(tag: str, data: bytes) -> bytes:
    """BIP340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data)"""
    tag_hash = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(tag_hash + tag_hash + data).digest()

def hash160(data: bytes) -> bytes:
    """RIPEMD160(SHA256(data)) - for P2PKH/P2WPKH addresses."""
    sha = hashlib.sha256(data).digest()
    ripemd = hashlib.new('ripemd160', sha).digest()
    return ripemd

# =============================================================================
# ECDSA Signing (for legacy/segwit v0)
# =============================================================================

def sign_ecdsa(privkey: bytes, sighash: bytes, deterministic: bool = True) -> bytes:
    """
    ECDSA signature (DER encoded).

    Args:
        privkey: 32-byte private key
        sighash: 32-byte message hash (transaction sighash)
        deterministic: Use RFC6979 deterministic k (recommended)

    Returns:
        DER-encoded signature (without sighash type byte)
    """
    privkey_int = int.from_bytes(privkey, 'big')
    z = int.from_bytes(sighash, 'big')

    # Generate k (nonce)
    if deterministic:
        k = _rfc6979_k(privkey, sighash)
    else:
        k = secrets.randbelow(N - 1) + 1

    # Compute signature
    point = point_mul(k)
    r = point[0] % N

    if r == 0:
        raise ValueError("Invalid k, r is zero")

    k_inv = modinv(k, N)
    s = (k_inv * (z + r * privkey_int)) % N

    if s == 0:
        raise ValueError("Invalid k, s is zero")

    # Use low-s (BIP62)
    if s > N // 2:
        s = N - s

    # DER encode
    return _der_encode_signature(r, s)

def _rfc6979_k(privkey: bytes, message: bytes) -> int:
    """RFC6979 deterministic k generation."""
    v = b'\x01' * 32
    k = b'\x00' * 32

    k = hmac.new(k, v + b'\x00' + privkey + message, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b'\x01' + privkey + message, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()

    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        candidate = int.from_bytes(v, 'big')
        if 0 < candidate < N:
            return candidate
        k = hmac.new(k, v + b'\x00', hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()

def _der_encode_signature(r: int, s: int) -> bytes:
    """DER encode (r, s) signature."""
    def encode_int(val: int) -> bytes:
        b = val.to_bytes((val.bit_length() + 7) // 8, 'big')
        if b[0] & 0x80:  # Add padding if high bit set
            b = b'\x00' + b
        return bytes([0x02, len(b)]) + b

    r_bytes = encode_int(r)
    s_bytes = encode_int(s)
    payload = r_bytes + s_bytes

    return bytes([0x30, len(payload)]) + payload

# =============================================================================
# Schnorr Signing (BIP340, for Taproot)
# =============================================================================

def sign_schnorr(privkey: bytes, message: bytes, aux_rand: bytes = None) -> bytes:
    """
    BIP340 Schnorr signature.

    Args:
        privkey: 32-byte private key
        message: 32-byte message (sighash)
        aux_rand: 32-byte auxiliary randomness (default: zeros)

    Returns:
        64-byte Schnorr signature
    """
    if aux_rand is None:
        aux_rand = b'\x00' * 32

    d = int.from_bytes(privkey, 'big')

    if d == 0 or d >= N:
        raise ValueError("Invalid private key")

    # Get public key point
    P = point_mul(d)
    px = P[0]
    py = P[1]

    # If y is odd, negate private key
    if py % 2 != 0:
        d = N - d

    # Generate k
    t = (d ^ int.from_bytes(tagged_hash("BIP0340/aux", aux_rand), 'big')).to_bytes(32, 'big')
    rand = tagged_hash("BIP0340/nonce", t + px.to_bytes(32, 'big') + message)
    k = int.from_bytes(rand, 'big') % N

    if k == 0:
        raise ValueError("k is zero")

    # R = k*G
    R = point_mul(k)
    rx = R[0]
    ry = R[1]

    # If R.y is odd, negate k
    if ry % 2 != 0:
        k = N - k

    # e = hash(R.x || P.x || message)
    e_bytes = tagged_hash("BIP0340/challenge",
                          rx.to_bytes(32, 'big') + px.to_bytes(32, 'big') + message)
    e = int.from_bytes(e_bytes, 'big') % N

    # s = k + e*d
    s = (k + e * d) % N

    # Signature: R.x || s (64 bytes)
    sig = rx.to_bytes(32, 'big') + s.to_bytes(32, 'big')

    return sig

def verify_schnorr(pubkey: bytes, message: bytes, signature: bytes) -> bool:
    """
    Verify BIP340 Schnorr signature.

    Args:
        pubkey: 32-byte x-only public key
        message: 32-byte message
        signature: 64-byte signature

    Returns:
        True if valid
    """
    if len(pubkey) != 32 or len(signature) != 64:
        return False

    px = int.from_bytes(pubkey, 'big')
    rx = int.from_bytes(signature[:32], 'big')
    s = int.from_bytes(signature[32:], 'big')

    if px >= P or rx >= P or s >= N:
        return False

    # Lift x to point P
    py_sq = (pow(px, 3, P) + 7) % P
    py = pow(py_sq, (P + 1) // 4, P)
    if pow(py, 2, P) != py_sq:
        return False
    if py % 2 != 0:
        py = P - py
    P_point = (px, py)

    # e = hash(R.x || P.x || message)
    e = int.from_bytes(tagged_hash("BIP0340/challenge",
                                    signature[:32] + pubkey + message), 'big') % N

    # R = s*G - e*P
    sG = point_mul(s)
    eP = point_mul(e, P_point)
    eP_neg = (eP[0], P - eP[1]) if eP else None
    R = point_add(sG, eP_neg)

    if R is None:
        return False

    # Check R.x == rx and R.y is even
    return R[0] == rx and R[1] % 2 == 0
