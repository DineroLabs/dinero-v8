# Taproot Key-Path Signing: Why Keypair APIs Are Required

## Background

BIP340/BIP341 Taproot uses x-only public keys (32 bytes) which implicitly represent
the even-Y coordinate point. When tweaking keys for key-path spending, the output
key's Y parity affects how signing must be performed.

## The Problem: Secret Key Extraction is Invalid

### What Doesn't Work

```cpp
// WRONG: Attempting to extract tweaked secret key
secp256k1_keypair keypair;
secp256k1_keypair_create(ctx, &keypair, internal_privkey);
secp256k1_keypair_xonly_tweak_add(ctx, &keypair, tweak);

// This extraction is INVALID:
uint8_t tweaked_privkey[32];
memcpy(tweaked_privkey, keypair.data, 32);  // DON'T DO THIS

// Signing with extracted key fails ~50% of the time
secp256k1_schnorrsig_sign32(ctx, sig, msg, &new_keypair, aux);
```

### Why It Fails

1. **Opaque Structure**: `secp256k1_keypair` is intentionally opaque. Its internal
   layout is implementation-specific and not guaranteed across versions.

2. **Y Parity Handling**: `secp256k1_keypair_xonly_tweak_add` doesn't store the
   "negated if odd-Y" secret key in a directly extractable location. The library
   handles parity internally during signing operations.

3. **Statistical Evidence**: ~50% of tweaked keys have odd-Y, causing signature
   verification failures when using the extracted (non-negated) secret key.

## The Solution: Sign Directly with Keypair

### Correct Implementation

```cpp
// CORRECT: Sign directly with tweaked keypair
secp256k1_keypair keypair;
secp256k1_keypair_create(ctx, &keypair, internal_privkey);
secp256k1_keypair_xonly_tweak_add(ctx, &keypair, tweak);

// Sign directly - library handles Y parity internally
secp256k1_schnorrsig_sign32(ctx, sig, msg, &keypair, aux);  // Always works
```

### Why It Works

`secp256k1_schnorrsig_sign32` uses the keypair's internal representation which
correctly tracks whether negation is needed. The library applies the negation
during the signing computation without exposing it externally.

## DineroCoin API

### For Key-Path Signing (Recommended)

```cpp
// Sign with internal key - handles tweak + Y parity internally
TaprootKeys::SignSchnorrWithInternalKey(
    signature,           // Output: 64-byte signature
    sighash,            // Input: 32-byte message hash
    internal_privkey,   // Input: 32-byte internal private key
    internal_xonly_pubkey,  // Input: 32-byte internal x-only pubkey
    aux_rand            // Optional: auxiliary randomness
);
```

### For Computing Output Pubkey (scriptPubKey)

```cpp
// Compute tweaked pubkey for P2TR scriptPubKey
TaprootKeys::ComputeTweakedPubkey(
    internal_xonly_pubkey,  // Input: 32-byte internal x-only pubkey
    tweaked_xonly_pubkey    // Output: 32-byte tweaked x-only pubkey
);
```

### Deprecated (Do Not Use for Signing)

```cpp
// DEPRECATED: Cannot correctly extract tweaked secret key
TaprootKeys::TweakPrivkey(privkey, xonly_pubkey);  // Compiler warning
```

## References

- **BIP340**: Schnorr Signatures for secp256k1
  - Section on x-only pubkeys and implicit Y coordinate

- **BIP341**: Taproot: SegWit version 1 spending rules
  - Key-path spending requires signing with tweaked key

- **libsecp256k1 API**:
  - `secp256k1_keypair_xonly_tweak_add`: Tweaks keypair in place
  - `secp256k1_schnorrsig_sign32`: Signs using keypair (handles parity)

## Test Coverage

The fix is validated by:
- `test_psbt_p2tr_roundtrip`: 100/100 random keys, ~50% odd-Y internal keys
- `test_taproot_consensus`: BIP340/341 compatibility
- `test_psbt_guardrails`: PSBT signing policy enforcement

## History

This design was implemented after discovering that ~50% of Taproot signatures
failed verification due to incorrect Y parity handling. The root cause was
attempting to extract the tweaked secret key from the opaque keypair structure.

The fix aligns DineroCoin with Bitcoin Core's security model: never expose
tweaked secret keys, always sign directly with keypair APIs.
