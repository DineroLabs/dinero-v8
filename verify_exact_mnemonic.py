#!/usr/bin/env python3
"""
DineroCoin BIP84 Address Verification
Test with EXACT mnemonic from test document
"""

from hashlib import sha256, pbkdf2_hmac
from hmac import new as hmac_new
import hashlib

# Test mnemonic from your test document
MNEMONIC = "romance maid able movie harsh hedgehog buyer shoulder wagon patrol fury practice"

def bip39_to_seed(mnemonic, passphrase=""):
    """BIP39: Mnemonic to 512-bit seed"""
    mnemonic_bytes = mnemonic.encode('utf-8')
    salt = ('mnemonic' + passphrase).encode('utf-8')
    return pbkdf2_hmac('sha512', mnemonic_bytes, salt, 2048)

def derive_bip32_master(seed):
    """BIP32: Derive master key from seed"""
    h = hmac_new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    return h[:32], h[32:64]  # private_key, chain_code

print("🧪 DineroCoin Address Verification")
print("=" * 50)
print(f"Mnemonic: {MNEMONIC}")
print()

# Generate seed
seed = bip39_to_seed(MNEMONIC)
print(f"Seed (first 32 bytes): {seed[:32].hex()}")

# Derive master key
priv, chain = derive_bip32_master(seed)
print(f"Master Private Key: {priv.hex()}")
print(f"Master Chain Code:  {chain.hex()}")

print()
print("⚠️  Note: Full BIP84 derivation (m/84'/1447'/0'/0/0) requires secp256k1")
print("Install: pip3 install coincurve")
print()
print("Expected Address (from test doc):")
print("  din1qpevgvx388zj87q7frenc5llvvma87504ll2jnr")
print()
print("Run this with coincurve installed for full verification")

