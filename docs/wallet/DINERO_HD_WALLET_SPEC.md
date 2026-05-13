# Dinero HD Wallet Derivation Specification

## Status: NORMATIVE

This document is the canonical reference for all BIP-32 derivation paths
used by Dinero wallets. Any wallet implementation (desktop, mobile, hardware)
MUST follow these paths to ensure cross-wallet recovery compatibility.

---

## Coin Type

```
coin_type = 1448'
```

All Dinero derivation paths use coin type 1448 (hardened). This is the
single chain identity. There is no secondary coin type.

**Legacy:** Coin type 1447 was used in early development builds. Wallets
MUST scan 1447 paths for existing funds on restore, but MUST NOT generate
new addresses under 1447. See Migration section below.

---

## Purpose Codes

Each cryptographic scheme gets its own BIP-43 purpose code:

| Purpose | Scheme | Script Type | Witness | Reference |
|---------|--------|-------------|---------|-----------|
| 86'     | secp256k1 (Schnorr) | P2TR (Taproot) | v1 | BIP-86 |
| 88'     | ML-DSA-65 (FIPS 204) | P2MR | v3 | Dinero v7 |
| 77'     | ZK nullifier model | Shielded | — | Dinero v7 |

**Why separate purposes?** Different cryptographic schemes produce
different key material from the same seed. Mixing them under one purpose
would create ambiguity during wallet recovery — the scanner wouldn't know
which key derivation function to apply at each index.

---

## Full Derivation Paths

### Taproot (P2TR)

```
m / 86' / 1448' / account' / change / index
```

- Key derivation: BIP-32 → x-only pubkey → Taproot tweak
- Address prefix: `din1p` (mainnet), `rdin1p` (regtest)
- Witness: v1, 64-byte Schnorr signature
- VWU: ~66 per input

### P2MR (Post-Quantum)

```
m / 88' / 1448' / account' / change / index
```

- Key derivation: BIP-32 → HKDF-SHA256 → ML-DSA-65 KeygenFromSeed
- Address prefix: `din1r` (mainnet), `rdin1r` (regtest)  
- Witness: v3, ~5.3 KB ML-DSA-65 signature
- VWU: ~5000 per input

### Shielded (ZK Private)

```
m / 77' / 1448' / account' / change / index
```

- Key derivation: BIP-32 → spending key (32 bytes)
- Public key: Poseidon(spending_key, 0)
- Note commitment: Poseidon(Poseidon(value, pk), randomness)
- Nullifier: Poseidon(spending_key, leaf_index)
- No on-chain address (commitments only)
- VWU: 5000 per shielded spend, 500 per shielded output

---

## Account, Change, Index

Standard BIP-44 semantics apply to all three purposes:

| Field | Range | Meaning |
|-------|-------|---------|
| account' | 0' - 2^31 | Hardened account isolation |
| change | 0 | External (receiving) |
| change | 1 | Internal (change) |
| index | 0 - 2^31 | Sequential address index |

Wallets SHOULD use account 0 by default. Multi-account support is
OPTIONAL but MUST follow this structure if implemented.

---

## Wallet Recovery (Gap Limit)

On seed restore, the wallet MUST scan:

1. **Purpose 86 (Taproot):** gap limit 20 (standard BIP-44)
2. **Purpose 88 (P2MR):** gap limit 20
3. **Purpose 77 (Shielded):** gap limit 20

For each purpose, scan account 0 first. If any address at account 0
has received funds, also scan account 1, and so on.

**Legacy scan:** Also scan purpose 86 under coin type 1447 (the old
development coin type). If funds are found, present them as "legacy"
and offer migration to 1448.

**Legacy P2MR scan:** Also scan purpose 44 under coin type 1448 for
early P2MR addresses generated before the purpose-88 standardization.

---

## Migration: 1447 → 1448

### Policy: Clean Break (Option A)

- **Effective immediately.** No new addresses are generated under 1447.
- Existing 1447 UTXOs remain spendable under their original derivation.
- `wallet.getnewaddress` always derives from 1448.
- `wallet.listunspent` shows both 1447 and 1448 UTXOs.
- Users are encouraged to send 1447 funds to a new 1448 address.

### Implementation

The wallet's `getNextAddressIndex()` and `getNewAddress()` use 1448.
The `LoadAddressesIntoUTXOIndex()` on wallet unlock scans BOTH 1447
and 1448 paths. No code change is needed for spending — the signer
resolves the derivation path from the UTXO's stored path, which
includes the full `m/86'/1447'/...` or `m/86'/1448'/...`.

---

## Encoding Summary

| Address | Prefix | Witness | Purpose | Coin Type |
|---------|--------|---------|---------|-----------|
| Taproot | din1p | v1 | 86' | 1448' |
| P2MR | din1r | v3 | 88' | 1448' |
| Shielded | (none) | — | 77' | 1448' |
| Legacy Taproot | din1p | v1 | 86' | 1447' (scan only) |
| Legacy P2MR | din1r | v3 | 44' | 1448' (scan only) |

---

## Shielded Recipient Encoding

The retired `dina1...` payment-code lineage is not part of the active
wallet surface and MUST NOT be generated or accepted by current Dinero
wallets.

Shielded spends still derive from:

```
m / 77' / 1448' / account' / change / index
```

but the user-facing recipient encoding for those keys is intentionally
left unspecified here until the shielded receive format is repinned.

Consensus only requires that a v5 shielded output carry:

- a note commitment
- an `encrypted_note` payload
- a valid proof under the active shielded rules

The exact recipient string format can evolve separately from the HD
derivation tree until a replacement for the retired `dina1...` format is
finalized.

---

## One-Line Summary

All Dinero addresses derive from `m / purpose' / 1448' / account' / change / index`
where purpose = 86 (Taproot), 88 (P2MR), or 77 (Shielded).
