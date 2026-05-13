#!/usr/bin/env python3
"""
Dinero P2WPKH Generator
======================
Generates P2WPKH scriptPubKey and bech32 address from compressed pubkey
Handles RIPEMD-160 fallback for macOS compatibility
"""

import sys, binascii, hashlib

# --- minimal bech32 (BIP-173) helpers ---
CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"

def bech32_polymod(values):
    g = [0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3]
    chk = 1
    for v in values:
        b = chk >> 25
        chk = ((chk & 0x1ffffff) << 5) ^ v
        for i in range(5):
            chk ^= g[i] if ((b >> i) & 1) else 0
    return chk

def bech32_hrp_expand(hrp): 
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]

def bech32_create_checksum(hrp, data):
    polymod = bech32_polymod(bech32_hrp_expand(hrp) + data + [0,0,0,0,0,0]) ^ 1
    return [(polymod >> 5*(5-i)) & 31 for i in range(6)]

def bech32_encode(hrp, data): 
    return hrp + '1' + ''.join(CHARSET[d] for d in data + bech32_create_checksum(hrp, data))

def convertbits(data, frombits, tobits, pad=True):
    acc, bits, ret, maxv = 0, 0, [], (1 << tobits) - 1
    for value in data:
        if value < 0 or value >> frombits: return None
        acc = (acc << frombits) | value; bits += frombits
        while bits >= tobits:
            bits -= tobits; ret.append((acc >> bits) & maxv)
    if pad:
        if bits: ret.append((acc << (tobits - bits)) & maxv)
    elif bits >= frombits or ((acc << (tobits - bits)) & maxv): return None
    return ret

def hash160(pub_bytes: bytes) -> bytes:
    """HASH160 = RIPEMD160(SHA256(data)) with fallback for macOS"""
    sha = hashlib.sha256(pub_bytes).digest()
    try:
        # Try built-in RIPEMD160
        rip = hashlib.new('ripemd160')
        rip.update(sha)
        return rip.digest()
    except ValueError:
        # Fallback to PyCryptodome
        try:
            from Crypto.Hash import RIPEMD
            return RIPEMD.new(sha).digest()
        except ImportError:
            print("❌ RIPEMD160 not available!")
            print("Install PyCryptodome: pip install pycryptodome")
            sys.exit(1)

def p2wpkh_spk(pub_hex: str) -> bytes:
    """Create P2WPKH scriptPubKey from compressed pubkey"""
    pub = bytes.fromhex(pub_hex)
    if len(pub) != 33 or pub[0] not in (0x02, 0x03):
        raise SystemExit("Need a 33-byte compressed secp256k1 pubkey (starts 02/03).")
    h160 = hash160(pub)
    return b"\x00\x14" + h160  # OP_0 (0x00), push 20 bytes (0x14), HASH160

def bech32_addr(hrp: str, spk: bytes) -> str:
    """Create bech32 address from P2WPKH scriptPubKey"""
    # spk = 00 14 <20-byte-hash> for P2WPKH → version=0, program=20 bytes
    assert spk[0] == 0 and spk[1] == 20
    prog = spk[2:]
    data = [0] + convertbits(prog, 8, 5)  # witness v0
    return bech32_encode(hrp, data)

def main():
    import json
    
    if len(sys.argv) < 3:
        print("Usage: gen_p2wpkh.py <33-byte-compressed-pubkey-hex> <hrp: din|rdin> [--json]")
        print("")
        print("Example:")
        print("  python3 gen_p2wpkh.py 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 din")
        print("  python3 gen_p2wpkh.py 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 din --json")
        sys.exit(1)
    
    pub_hex, hrp = sys.argv[1], sys.argv[2]
    json_output = "--json" in sys.argv
    
    try:
        spk = p2wpkh_spk(pub_hex)
        h160 = spk[2:]
        
        out = {
            "compressed_pubkey": pub_hex,
            "hash160": h160.hex(),
            "scriptPubKey_hex": spk.hex(),
            "bech32_address": bech32_addr(hrp, spk)
        }
        
        if json_output:
            print(json.dumps(out))
        else:
            print(f"🔑 P2WPKH Generation for HRP: {hrp}")
            print("=" * 40)
            print(f"Compressed pubkey   : {out['compressed_pubkey']}")
            print(f"HASH160(pub)        : {out['hash160']}")
            print(f"scriptPubKey hex    : {out['scriptPubKey_hex']}")        # embed this in genesis vout
            print(f"bech32 address      : {out['bech32_address']}")
            print("")
            print("✅ Ready for genesis integration!")
        
    except Exception as e:
        print(f"❌ Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
