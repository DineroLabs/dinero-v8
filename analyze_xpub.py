#!/usr/bin/env python3
import json
import sys
import base58

# Sample DineroCoin xpub from export
dinero_xpub = "7XPnQvuv6QdeJa1nnqnC2mb3PWkLDkCFYJjxSpDRxNA6HK2z5Bk3V5fMZHfb7d59dVNiQqNmaiUm2tDSFRnMi3W2hfaUMPRiH38XBjMDGr6WWVthE1qVV"

# Standard BIP32 xpub for comparison
standard_xpub = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8"

print("DineroCoin Custom Format Analysis")
print("=" * 60)
print(f"DineroCoin xpub: {dinero_xpub}")
print(f"String length: {len(dinero_xpub)} chars")

try:
    dinero_decoded = base58.b58decode(dinero_xpub)
    print(f"Decoded length: {len(dinero_decoded)} bytes")
    print(f"First 4 bytes (version): {dinero_decoded[:4].hex()}")
    print(f"Decoded hex: {dinero_decoded.hex()}")
except Exception as e:
    print(f"Decode error: {e}")

print("\n" + "=" * 60)
print(f"Standard BIP32 xpub: {standard_xpub}")
print(f"String length: {len(standard_xpub)} chars")

try:
    standard_decoded = base58.b58decode(standard_xpub)
    print(f"Decoded length: {len(standard_decoded)} bytes (BIP32 standard)")
    print(f"First 4 bytes (version): {standard_decoded[:4].hex()}")
except Exception as e:
    print(f"Decode error: {e}")

print("\n" + "=" * 60)
print("Difference:")
print(f"DineroCoin is {len(dinero_decoded) - len(standard_decoded)} bytes longer than standard")
print(f"\nStandard BIP32 format (78 bytes):")
print("  version(4) + depth(1) + parent_fpr(4) + child_num(4)")
print("  + chain_code(32) + pubkey(33) = 78 bytes")
print(f"\nDineroCoin format ({len(dinero_decoded)} bytes):")
print("  Likely includes additional metadata (network byte, checksum variant, etc.)")
