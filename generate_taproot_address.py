#!/usr/bin/env python3
"""
Generate a valid DineroCoin Taproot (bech32m) test address
"""

# Bech32m implementation (from BIP350)
def bech32_polymod(values):
    GEN = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
    chk = 1
    for v in values:
        b = chk >> 25
        chk = (chk & 0x1ffffff) << 5 ^ v
        for i in range(5):
            chk ^= GEN[i] if ((b >> i) & 1) else 0
    return chk

def bech32_hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]

def bech32_verify_checksum(hrp, data, const):
    return bech32_polymod(bech32_hrp_expand(hrp) + data) == const

def bech32_create_checksum(hrp, data, const):
    values = bech32_hrp_expand(hrp) + data
    polymod = bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ const
    return [(polymod >> 5 * (5 - i)) & 31 for i in range(6)]

def bech32_encode(hrp, witver, witprog, const):
    """Encode a segwit address."""
    data = [witver] + convertbits(witprog, 8, 5)
    if data is None:
        return None
    combined = data + bech32_create_checksum(hrp, data, const)
    return hrp + '1' + ''.join([CHARSET[d] for d in combined])

def convertbits(data, frombits, tobits, pad=True):
    """General power-of-2 base conversion."""
    acc = 0
    bits = 0
    ret = []
    maxv = (1 << tobits) - 1
    max_acc = (1 << (frombits + tobits - 1)) - 1
    for value in data:
        if value < 0 or (value >> frombits):
            return None
        acc = ((acc << frombits) | value) & max_acc
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

CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
BECH32M_CONST = 0x2bc830a3

def generate_taproot_address():
    """Generate a valid DineroCoin Taproot address for regtest"""

    # DineroCoin uses "din" for all networks (mainnet, testnet, regtest)
    hrp = "din"

    # Test 32-byte x-only pubkey (same as BIP340 test vector for consistency)
    xonly_pubkey_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    xonly_pubkey = bytes.fromhex(xonly_pubkey_hex)

    # Witness version 1 (Taproot)
    witver = 1

    # Encode as bech32m
    address = bech32_encode(hrp, witver, list(xonly_pubkey), BECH32M_CONST)

    return address, xonly_pubkey_hex

if __name__ == "__main__":
    address, pubkey = generate_taproot_address()

    print("=" * 60)
    print("DineroCoin Taproot Test Address Generator")
    print("=" * 60)
    print()
    print(f"Network:           regtest")
    print(f"HRP:               din (all networks use 'din')")
    print(f"Witness Version:   1 (Taproot)")
    print(f"Encoding:          bech32m")
    print()
    print(f"X-only pubkey:     {pubkey}")
    print()
    print(f"Taproot Address:   {address}")
    print()
    print("=" * 60)
    print("Test with:")
    print(f"  ./build/bin/dinero-cli wallet.sendtoaddress {address} 1.5")
    print("=" * 60)
