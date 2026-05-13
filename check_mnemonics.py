#!/usr/bin/env python3
"""
BIP86 Taproot address derivation for Dinero (coin_type=1447, HRP="din").
Pure Python implementation using hashlib, hmac, struct, and the ecdsa library.
"""

import hashlib
import hmac
import struct
import unicodedata

# secp256k1 curve parameters
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
Gx = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
Gy = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

# ---- EC point arithmetic on secp256k1 ----

def modinv(a, m=P):
    return pow(a, m - 2, m)

def point_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and y1 == y2:
        lam = (3 * x1 * x1) * modinv(2 * y1) % P
    elif x1 == x2:
        return None
    else:
        lam = (y2 - y1) * modinv(x2 - x1) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return (x3, y3)

def point_mul(k, point=None):
    if point is None:
        point = (Gx, Gy)
    result = None
    addend = point
    while k:
        if k & 1:
            result = point_add(result, addend)
        addend = point_add(addend, addend)
        k >>= 1
    return result

def point_from_privkey(privkey_int):
    return point_mul(privkey_int)

def serialize_point_compressed(point):
    x, y = point
    prefix = b'\x02' if y % 2 == 0 else b'\x03'
    return prefix + x.to_bytes(32, 'big')

def lift_x(x_int):
    """Lift x coordinate to a point (pick even y)."""
    x = x_int % P
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    if y % 2 != 0:
        y = P - y
    return (x, y)

# ---- BIP39 Mnemonic to Seed ----

BIP39_WORDLIST_URL = None  # We'll embed a minimal approach

def get_bip39_wordlist():
    """Load the English BIP39 wordlist."""
    # Try to load from a file or use hashlib-based validation
    # For this script, we just need mnemonic->seed which doesn't require the wordlist
    # (PBKDF2 doesn't validate words)
    pass

def mnemonic_to_seed(mnemonic, passphrase=""):
    """BIP39: mnemonic + passphrase -> 64-byte seed via PBKDF2."""
    mnemonic_normalized = unicodedata.normalize("NFKD", mnemonic)
    passphrase_normalized = unicodedata.normalize("NFKD", "mnemonic" + passphrase)
    return hashlib.pbkdf2_hmac(
        "sha512",
        mnemonic_normalized.encode("utf-8"),
        passphrase_normalized.encode("utf-8"),
        2048,
        dklen=64
    )

# ---- BIP32 HD Key Derivation ----

def hmac_sha512(key, data):
    return hmac.new(key, data, hashlib.sha512).digest()

def parse_256(b):
    return int.from_bytes(b, 'big')

class BIP32Key:
    def __init__(self, privkey, chaincode, depth=0, parent_fingerprint=b'\x00\x00\x00\x00', child_index=0):
        self.privkey = privkey  # 32-byte private key
        self.chaincode = chaincode  # 32-byte chain code
        self.depth = depth
        self.parent_fingerprint = parent_fingerprint
        self.child_index = child_index

    @classmethod
    def from_seed(cls, seed):
        I = hmac_sha512(b"Bitcoin seed", seed)
        IL, IR = I[:32], I[32:]
        key_int = parse_256(IL)
        if key_int == 0 or key_int >= N:
            raise ValueError("Invalid master key")
        return cls(IL, IR)

    def get_pubkey_compressed(self):
        key_int = parse_256(self.privkey)
        point = point_from_privkey(key_int)
        return serialize_point_compressed(point)

    def fingerprint(self):
        pubkey = self.get_pubkey_compressed()
        h = hashlib.new('ripemd160', hashlib.sha256(pubkey).digest()).digest()
        return h[:4]

    def derive_child(self, index):
        """Derive child key. index >= 0x80000000 for hardened."""
        if index >= 0x80000000:
            # Hardened: HMAC-SHA512(Key=chaincode, Data=0x00||privkey||index)
            data = b'\x00' + self.privkey + struct.pack('>I', index)
        else:
            # Normal: HMAC-SHA512(Key=chaincode, Data=pubkey||index)
            data = self.get_pubkey_compressed() + struct.pack('>I', index)

        I = hmac_sha512(self.chaincode, data)
        IL, IR = I[:32], I[32:]

        il_int = parse_256(IL)
        key_int = parse_256(self.privkey)
        child_key_int = (il_int + key_int) % N

        if il_int >= N or child_key_int == 0:
            raise ValueError("Invalid child key")

        child_privkey = child_key_int.to_bytes(32, 'big')

        return BIP32Key(
            child_privkey,
            IR,
            depth=self.depth + 1,
            parent_fingerprint=self.fingerprint(),
            child_index=index
        )

    def derive_path(self, path):
        """Derive from path string like m/86'/1447'/0'/0/0."""
        if path.startswith('m/'):
            path = path[2:]
        elif path == 'm':
            return self

        key = self
        for component in path.split('/'):
            if component.endswith("'") or component.endswith("h"):
                index = int(component[:-1]) + 0x80000000
            else:
                index = int(component)
            key = key.derive_child(index)
        return key

# ---- BIP341/BIP86 Taproot ----

def tagged_hash(tag, data):
    """BIP340 tagged hash: SHA256(SHA256(tag)||SHA256(tag)||data)."""
    tag_hash = hashlib.sha256(tag.encode('utf-8')).digest()
    return hashlib.sha256(tag_hash + tag_hash + data).digest()

def bip86_tweak_pubkey(internal_pubkey_xonly):
    """
    BIP86: tweak = tagged_hash("TapTweak", internal_pubkey_xonly)
    Output key = internal_key + tweak*G
    Returns (x-only output key, parity).
    """
    tweak = tagged_hash("TapTweak", internal_pubkey_xonly)
    tweak_int = parse_256(tweak)

    if tweak_int >= N:
        raise ValueError("Tweak out of range")

    # Lift the x-only internal key to a point (even y)
    x_int = parse_256(internal_pubkey_xonly)
    internal_point = lift_x(x_int)
    if internal_point is None:
        raise ValueError("Invalid internal public key")

    # tweak_point = tweak * G
    tweak_point = point_mul(tweak_int)

    # output_point = internal_point + tweak_point
    output_point = point_add(internal_point, tweak_point)
    if output_point is None:
        raise ValueError("Output point is at infinity")

    x_out, y_out = output_point
    parity = y_out % 2

    return x_out.to_bytes(32, 'big'), parity

# ---- Bech32m Encoding ----

BECH32M_CONST = 0x2bc830a3

def bech32_polymod(values):
    GEN = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
    chk = 1
    for v in values:
        b = (chk >> 25)
        chk = (chk & 0x1ffffff) << 5 ^ v
        for i in range(5):
            chk ^= GEN[i] if ((b >> i) & 1) else 0
    return chk

def bech32_hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]

def bech32m_create_checksum(hrp, data):
    values = bech32_hrp_expand(hrp) + data
    polymod = bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ BECH32M_CONST
    return [(polymod >> 5 * (5 - i)) & 31 for i in range(6)]

def bech32m_encode(hrp, witver, witprog):
    """Encode a segwit address using bech32m."""
    CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
    # Convert witness program to 5-bit groups
    data = convertbits(witprog, 8, 5)
    combined = [witver] + data
    checksum = bech32m_create_checksum(hrp, combined)
    return hrp + "1" + "".join([CHARSET[d] for d in combined + checksum])

def convertbits(data, frombits, tobits, pad=True):
    """General power-of-2 base conversion."""
    acc = 0
    bits = 0
    ret = []
    maxv = (1 << tobits) - 1
    for value in data:
        if value < 0 or (value >> frombits):
            return None
        acc = (acc << frombits) | value
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            ret.append((acc >> bits) & maxv)
    if pad:
        if bits:
            ret.append((acc << (tobits - bits)) & maxv)
    elif bits >= frombits or ((acc << (tobits - bits)) & maxv):
        return None
    return ret

# ---- Main Logic ----

def derive_bip86_address(master_key, account, change, index, hrp="din"):
    """Derive a BIP86 Taproot address at m/86'/coin'/account'/change/index."""
    coin_type = 1447
    path = f"m/86'/{coin_type}'/{account}'/{change}/{index}"
    child_key = master_key.derive_path(path)

    # Get compressed public key
    pubkey_compressed = child_key.get_pubkey_compressed()

    # x-only internal key (drop the prefix byte)
    internal_key_xonly = pubkey_compressed[1:]

    # BIP86 tweak
    output_key_xonly, _ = bip86_tweak_pubkey(internal_key_xonly)

    # Encode as bech32m with witness version 1
    address = bech32m_encode(hrp, 1, list(output_key_xonly))

    return address, path, internal_key_xonly.hex(), output_key_xonly.hex()

def main():
    mnemonics = [
        ("Mnemonic A (bone->zone fix)", "knife wreck jar main humor zone hair summer century letter flag strong"),
        ("Mnemonic B (hair->air fix)", "knife wreck jar main humor bone air summer century letter flag strong"),
        ("Mnemonic C (user's other seed)", "sound riot cherry weapon dizzy copy hip symptom birth element horror color"),
    ]

    # Known mining addresses (prefixes)
    known_prefixes = [
        "din1p084lgt",
        "din1pqdy59f",
        "din1pns3pt6",
    ]

    # Known scriptPubKey -> address
    # scriptPubKey: 5120ca062bff883f6d1511a72a78a09f3a17cb6734f9ce8543faa66f7e380d58d1b7
    # The witness program is the 32 bytes after 5120
    known_spk_witness = "ca062bff883f6d1511a72a78a09f3a17cb6734f9ce8543faa66f7e380d58d1b7"
    known_spk_address = bech32m_encode("din", 1, list(bytes.fromhex(known_spk_witness)))
    print(f"Address from known scriptPubKey: {known_spk_address}")
    known_prefixes.append(known_spk_address[:12])
    print(f"Known address prefixes to search for: {known_prefixes}")
    print()

    all_matches = []

    for label, mnemonic in mnemonics:
        print(f"{'='*80}")
        print(f"  {label}")
        print(f"  Mnemonic: {mnemonic}")
        print(f"{'='*80}")

        seed = mnemonic_to_seed(mnemonic, "")
        print(f"  Seed: {seed.hex()}")

        master = BIP32Key.from_seed(seed)
        print(f"  Master privkey: {master.privkey.hex()}")
        print(f"  Master chaincode: {master.chaincode.hex()}")
        print()

        # Derive receive addresses (m/86'/1447'/0'/0/index)
        print(f"  --- Receive addresses (m/86'/1447'/0'/0/i) ---")
        for i in range(21):
            addr, path, internal_hex, output_hex = derive_bip86_address(master, 0, 0, i)
            match_flag = ""
            for prefix in known_prefixes:
                if addr.startswith(prefix):
                    match_flag = "  *** MATCH ***"
                    all_matches.append((label, path, addr, prefix))
            print(f"  [{i:2d}] {addr}  {path}{match_flag}")

        print()

        # Derive change addresses (m/86'/1447'/0'/1/index)
        print(f"  --- Change addresses (m/86'/1447'/0'/1/i) ---")
        for i in range(6):
            addr, path, internal_hex, output_hex = derive_bip86_address(master, 0, 1, i)
            match_flag = ""
            for prefix in known_prefixes:
                if addr.startswith(prefix):
                    match_flag = "  *** MATCH ***"
                    all_matches.append((label, path, addr, prefix))
            print(f"  [{i:2d}] {addr}  {path}{match_flag}")

        print()

    print(f"{'='*80}")
    print(f"  SUMMARY OF MATCHES")
    print(f"{'='*80}")
    if all_matches:
        for label, path, addr, prefix in all_matches:
            print(f"  {label}")
            print(f"    Path: {path}")
            print(f"    Address: {addr}")
            print(f"    Matched prefix: {prefix}")
            print()
    else:
        print("  NO MATCHES FOUND against any of the known mining addresses.")
        print()

    # Also check the full known address from scriptPubKey
    print(f"  Full address from scriptPubKey 5120{known_spk_witness}:")
    print(f"  {known_spk_address}")
    print()

    # Extra: also try account 1 and 2 for mnemonic C (in case different account was used)
    print(f"{'='*80}")
    print(f"  EXTRA: Trying accounts 1-2 for Mnemonic C")
    print(f"{'='*80}")
    seed = mnemonic_to_seed(mnemonics[2][1], "")
    master = BIP32Key.from_seed(seed)
    for acct in [1, 2]:
        print(f"  --- Account {acct}, receive (m/86'/1447'/{acct}'/0/i) ---")
        for i in range(10):
            addr, path, _, _ = derive_bip86_address(master, acct, 0, i)
            match_flag = ""
            for prefix in known_prefixes:
                if addr.startswith(prefix):
                    match_flag = "  *** MATCH ***"
                    all_matches.append((mnemonics[2][0], path, addr, prefix))
            print(f"  [{i:2d}] {addr}  {path}{match_flag}")
        print()

if __name__ == "__main__":
    main()
