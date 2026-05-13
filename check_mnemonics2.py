#!/usr/bin/env python3
"""
BIP86 Taproot address derivation for Dinero — extended search.
Also includes the ORIGINAL mnemonic (before any word fixes),
and tries BIP84/BIP44 paths too (in case mining used non-BIP86).
Also tries coin_type 0 (Bitcoin) in case that's what the miner used.
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

def modinv(a, m=P):
    return pow(a, m - 2, m)

def point_add(p1, p2):
    if p1 is None: return p2
    if p2 is None: return p1
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

def serialize_point_compressed(point):
    x, y = point
    prefix = b'\x02' if y % 2 == 0 else b'\x03'
    return prefix + x.to_bytes(32, 'big')

def lift_x(x_int):
    x = x_int % P
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    if y % 2 != 0:
        y = P - y
    return (x, y)

def mnemonic_to_seed(mnemonic, passphrase=""):
    mnemonic_normalized = unicodedata.normalize("NFKD", mnemonic)
    passphrase_normalized = unicodedata.normalize("NFKD", "mnemonic" + passphrase)
    return hashlib.pbkdf2_hmac("sha512", mnemonic_normalized.encode("utf-8"),
                                passphrase_normalized.encode("utf-8"), 2048, dklen=64)

def hmac_sha512(key, data):
    return hmac.new(key, data, hashlib.sha512).digest()

def parse_256(b):
    return int.from_bytes(b, 'big')

class BIP32Key:
    def __init__(self, privkey, chaincode, depth=0, parent_fingerprint=b'\x00\x00\x00\x00', child_index=0):
        self.privkey = privkey
        self.chaincode = chaincode
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
        point = point_mul(key_int)
        return serialize_point_compressed(point)

    def fingerprint(self):
        pubkey = self.get_pubkey_compressed()
        h = hashlib.new('ripemd160', hashlib.sha256(pubkey).digest()).digest()
        return h[:4]

    def derive_child(self, index):
        if index >= 0x80000000:
            data = b'\x00' + self.privkey + struct.pack('>I', index)
        else:
            data = self.get_pubkey_compressed() + struct.pack('>I', index)
        I = hmac_sha512(self.chaincode, data)
        IL, IR = I[:32], I[32:]
        il_int = parse_256(IL)
        key_int = parse_256(self.privkey)
        child_key_int = (il_int + key_int) % N
        if il_int >= N or child_key_int == 0:
            raise ValueError("Invalid child key")
        child_privkey = child_key_int.to_bytes(32, 'big')
        return BIP32Key(child_privkey, IR, depth=self.depth + 1,
                       parent_fingerprint=self.fingerprint(), child_index=index)

    def derive_path(self, path):
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

def tagged_hash(tag, data):
    tag_hash = hashlib.sha256(tag.encode('utf-8')).digest()
    return hashlib.sha256(tag_hash + tag_hash + data).digest()

def bip86_tweak_pubkey(internal_pubkey_xonly):
    tweak = tagged_hash("TapTweak", internal_pubkey_xonly)
    tweak_int = parse_256(tweak)
    if tweak_int >= N:
        raise ValueError("Tweak out of range")
    x_int = parse_256(internal_pubkey_xonly)
    internal_point = lift_x(x_int)
    if internal_point is None:
        raise ValueError("Invalid internal public key")
    tweak_point = point_mul(tweak_int)
    output_point = point_add(internal_point, tweak_point)
    if output_point is None:
        raise ValueError("Output point is at infinity")
    x_out, y_out = output_point
    return x_out.to_bytes(32, 'big'), y_out % 2

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
    CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
    data = convertbits(witprog, 8, 5)
    combined = [witver] + data
    checksum = bech32m_create_checksum(hrp, combined)
    return hrp + "1" + "".join([CHARSET[d] for d in combined + checksum])

def convertbits(data, frombits, tobits, pad=True):
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

# BIP86 address derivation
def derive_bip86_address(master_key, coin_type, account, change, index, hrp="din"):
    path = f"m/86'/{coin_type}'/{account}'/{change}/{index}"
    child_key = master_key.derive_path(path)
    pubkey_compressed = child_key.get_pubkey_compressed()
    internal_key_xonly = pubkey_compressed[1:]  # drop prefix
    output_key_xonly, _ = bip86_tweak_pubkey(internal_key_xonly)
    address = bech32m_encode(hrp, 1, list(output_key_xonly))
    return address, path

# Also derive P2WPKH (BIP84) addresses for comparison — witness v0
def bech32_create_checksum(hrp, data):
    values = bech32_hrp_expand(hrp) + data
    polymod = bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ 1  # bech32 constant is 1
    return [(polymod >> 5 * (5 - i)) & 31 for i in range(6)]

def bech32_encode(hrp, witver, witprog):
    CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
    data = convertbits(witprog, 8, 5)
    combined = [witver] + data
    checksum = bech32_create_checksum(hrp, combined)
    return hrp + "1" + "".join([CHARSET[d] for d in combined + checksum])

def derive_bip84_address(master_key, coin_type, account, change, index, hrp="din"):
    """BIP84 P2WPKH: m/84'/coin'/account'/change/index"""
    path = f"m/84'/{coin_type}'/{account}'/{change}/{index}"
    child_key = master_key.derive_path(path)
    pubkey = child_key.get_pubkey_compressed()
    # P2WPKH witness program = HASH160(pubkey)
    h160 = hashlib.new('ripemd160', hashlib.sha256(pubkey).digest()).digest()
    address = bech32_encode(hrp, 0, list(h160))
    return address, path

def main():
    mnemonics = [
        ("Original mnemonic", "knife wreck jar main humor bone hair summer century letter flag strong"),
        ("Mnemonic A (bone->zone)", "knife wreck jar main humor zone hair summer century letter flag strong"),
        ("Mnemonic B (hair->air)", "knife wreck jar main humor bone air summer century letter flag strong"),
        ("Mnemonic C (other seed)", "sound riot cherry weapon dizzy copy hip symptom birth element horror color"),
    ]

    # Known mining addresses/prefixes
    known_prefixes = [
        "din1p084lgt",
        "din1pqdy59f",
        "din1pns3pt6",
    ]
    # From scriptPubKey
    known_spk = "ca062bff883f6d1511a72a78a09f3a17cb6734f9ce8543faa66f7e380d58d1b7"
    known_spk_addr = bech32m_encode("din", 1, list(bytes.fromhex(known_spk)))
    known_prefixes.append(known_spk_addr[:12])

    # Also check the treasury address from memory
    treasury = "din1pljx7yr8pcdrdxfx7qmqgnvlv4zsj7sg82zpvyraunyalllzsvzaqynrc80"
    known_prefixes.append(treasury[:12])

    print(f"Known prefixes to match: {known_prefixes}")
    print(f"ScriptPubKey address: {known_spk_addr}")
    print(f"Treasury address: {treasury}")
    print()

    # Build a full set of known addresses for exact match
    known_full = set()
    known_full.add(known_spk_addr)
    known_full.add(treasury)

    all_matches = []

    for label, mnemonic in mnemonics:
        print(f"\n{'='*80}")
        print(f"  {label}")
        print(f"  Words: {mnemonic}")
        print(f"{'='*80}")

        seed = mnemonic_to_seed(mnemonic, "")
        master = BIP32Key.from_seed(seed)

        # BIP86 Taproot with coin_type=1447
        print(f"\n  [BIP86 Taproot, coin=1447] Receive m/86'/1447'/0'/0/i:")
        for i in range(21):
            addr, path = derive_bip86_address(master, 1447, 0, 0, i)
            flag = ""
            for px in known_prefixes:
                if addr.startswith(px):
                    flag = " *** MATCH ***"
                    all_matches.append((label, path, addr, px))
            if addr in known_full:
                flag = " *** EXACT MATCH ***"
                all_matches.append((label, path, addr, "EXACT"))
            print(f"    [{i:2d}] {addr}{flag}")

        print(f"\n  [BIP86 Taproot, coin=1447] Change m/86'/1447'/0'/1/i:")
        for i in range(6):
            addr, path = derive_bip86_address(master, 1447, 0, 1, i)
            flag = ""
            for px in known_prefixes:
                if addr.startswith(px):
                    flag = " *** MATCH ***"
                    all_matches.append((label, path, addr, px))
            print(f"    [{i:2d}] {addr}{flag}")

        # BIP86 Taproot with coin_type=0 (Bitcoin default)
        print(f"\n  [BIP86 Taproot, coin=0] Receive m/86'/0'/0'/0/i:")
        for i in range(10):
            addr, path = derive_bip86_address(master, 0, 0, 0, i)
            flag = ""
            for px in known_prefixes:
                if addr.startswith(px):
                    flag = " *** MATCH ***"
                    all_matches.append((label, path, addr, px))
            print(f"    [{i:2d}] {addr}{flag}")

        # BIP84 P2WPKH with coin_type=1447
        print(f"\n  [BIP84 P2WPKH, coin=1447] Receive m/84'/1447'/0'/0/i:")
        for i in range(10):
            addr, path = derive_bip84_address(master, 1447, 0, 0, i)
            flag = ""
            for px in known_prefixes:
                if addr.startswith(px):
                    flag = " *** MATCH ***"
                    all_matches.append((label, path, addr, px))
            print(f"    [{i:2d}] {addr}{flag}")

    # FINAL SUMMARY
    print(f"\n\n{'='*80}")
    print(f"  MATCH SUMMARY")
    print(f"{'='*80}")
    if all_matches:
        for label, path, addr, px in all_matches:
            print(f"  {label} | {path} | {addr} | matched: {px}")
    else:
        print("  NO MATCHES FOUND across all mnemonics and derivation paths.")

    # Sanity check: verify our BIP86 implementation with a known Bitcoin test vector
    print(f"\n\n{'='*80}")
    print(f"  BIP86 IMPLEMENTATION SANITY CHECK")
    print(f"{'='*80}")
    # BIP86 test vector from the BIP:
    # Mnemonic: "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    # m/86'/0'/0'/0/0 -> bc1p5cyxnuxmeuwuvkwfem96lqzszee2t74gm...
    test_mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    test_seed = mnemonic_to_seed(test_mnemonic, "")
    test_master = BIP32Key.from_seed(test_seed)
    test_key = test_master.derive_path("m/86'/0'/0'/0/0")
    test_pubkey = test_key.get_pubkey_compressed()
    test_xonly = test_pubkey[1:]
    print(f"  Test m/86'/0'/0'/0/0 internal key (x-only): {test_xonly.hex()}")
    # BIP86 test vector: internal key should be a858eeb68db7f02edc4778c3a3bcfff2a4c39b5e7f3dc36e2f39cfacdcc8f112
    # From BIP86 spec: x-only pubkey = a858eeb68db7f02edc4778c3a3bcfff2a4c39b5e7f3dc36e2f39cfacdcc8f112
    expected_internal = "a858eeb68db7f02edc4778c3a3bcfff2a4c39b5e7f3dc36e2f39cfacdcc8f112"
    if test_xonly.hex() == expected_internal:
        print(f"  PASS: Internal key matches BIP86 test vector!")
    else:
        print(f"  FAIL: Expected {expected_internal}")
        print(f"  Got:           {test_xonly.hex()}")

    # Now check the tweaked output key
    test_output, _ = bip86_tweak_pubkey(test_xonly)
    print(f"  Test m/86'/0'/0'/0/0 output key: {test_output.hex()}")
    # Expected output key: a60869f0dbcf1dc659c9cecbee8900f3d6f0ece2bbc8c2f66dd5aabc891f3adc
    expected_output = "a60869f0dbcf1dc659c9cecbee8900f3d6f0ece2bbc8c2f66dd5aabc891f3adc"
    if test_output.hex() == expected_output:
        print(f"  PASS: Output key matches BIP86 test vector!")
    else:
        print(f"  FAIL: Expected {expected_output}")
        print(f"  Got:           {test_output.hex()}")

    # Expected BTC address: bc1p5cyxnuxmeuwuvkwfem96lqzszee2t74gm... let's check
    test_addr = bech32m_encode("bc", 1, list(test_output))
    expected_addr = "bc1p5cyxnuxmeuwuvkwfem96lqzszee2t74gm9zenez62vg9e4mjmlspsgepqm6"
    # From BIP86 spec, first receive address
    print(f"  Test address: {test_addr}")
    if test_addr[:20] == expected_addr[:20]:
        print(f"  PASS: Address prefix matches!")
    else:
        print(f"  FAIL: Expected prefix {expected_addr[:20]}")

if __name__ == "__main__":
    main()
