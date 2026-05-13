# Dinero Consensus Snapshot v1

**Status**: AUTHORITATIVE — all implementations MUST match this spec
**Date**: 2026-02-22
**Applies to**: Genesis reset (all consensus fixes frozen)

This document defines the exact consensus rules that are baked into genesis.
If you cannot reproduce the genesis header bytes from this spec, you cannot run the network.

---

## 1. Block Header (128 bytes)

```
Offset  Size  Field             Encoding
──────  ────  ────────────────  ────────────────────
0x00      4   version           uint32 LE
0x04     32   prev_block_hash   uint256 (raw bytes)
0x24     32   merkle_root       uint256 (raw bytes)
0x44     32   utreexo_root      uint256 (raw bytes)
0x64      8   timestamp         uint64 LE
0x6C      4   difficulty        uint32 LE (compact nBits)
0x70      4   nonce             uint32 LE
0x74     12   reserved          MUST be all zeros
```

Total: 128 bytes. `#pragma pack(push, 1)`, trivially copyable.

**Block hash**: `SHA256(SHA256(all_128_bytes))`.
SHA256 outputs big-endian. uint256 stores little-endian (data[0]=LSB).
Reversal: `result.data[i] = sha256_output[31 - i]`.
Display: `uint256::GetHex()` reverses again → leading zeros for PoW.

**Source**: `include/primitives/block.h`, `src/primitives/block.cpp:57-116`

---

## 2. Transaction Hashing

**Txid** (consensus): `SHA256(SHA256(Serialize(WithoutWitness)))`
**Wtxid** (not consensus-active): `SHA256(SHA256(Serialize(WithWitness)))`

Serialization format (all LE):
1. version (4)
2. [optional: segwit marker 0x00 + flag 0x01]
3. input count (CompactSize)
4. inputs: txid(32) + vout(4) + scriptSig(CompactSize+data) + sequence(4)
5. output count (CompactSize)
6. outputs: value(8) + scriptPubKey(CompactSize+data) [+ confidential fields]
7. [explicit fee if confidential]
8. [witness data if present]
9. locktime (4)

**Source**: `src/primitives/transaction.cpp:147-173`

---

## 3. Merkle Root

Standard Bitcoin merkle tree over txids:

1. Layer 0: `layer[i] = txid(tx[i])`
2. Single tx: merkle_root = txid
3. While layer > 1 element:
   - Odd count → duplicate last element
   - Pair hash: `SHA256(SHA256(left_32 || right_32))`
4. Return layer[0]

Uses `crypto::CSHA256` (not `Dinero::Common::double_sha256`).

**Source**: `src/consensus/merkle_root.cpp:14-58`

---

## 4. Taproot Tagged Hash (BIP340)

```
TaggedHash(tag, data) = SHA256(SHA256(tag) || SHA256(tag) || data)
```

- `tag`: ASCII string ("TapTweak", "TapLeaf", "TapBranch", etc.)
- `SHA256(tag)`: 32 bytes, cached per tag
- Preimage: tag_hash(32) || tag_hash(32) || data(variable)
- Single canonical implementation in `include/crypto/tagged_hash.h:19-27`
- All Taproot code MUST use `dinero::crypto::TaggedHash()`

---

## 5. Utreexo Accumulator

### 5.1 Leaf Hash (HashUTXO)

```
SHA256("DINERO-UTXO-LEAF-v1" || txid || vout_LE32 || amount_LE64 || CompactSize(script_len) || scriptPubKey)
```

| Field | Bytes | Encoding |
|-------|-------|----------|
| Domain tag | 19 | ASCII "DINERO-UTXO-LEAF-v1" |
| txid | 32 | Raw uint256 bytes |
| vout | 4 | Little-endian |
| amount | 8 | Little-endian (una) |
| script length | 1-5 | CompactSize varint |
| scriptPubKey | variable | Raw bytes |

CompactSize encoding:
- `< 253`: 1 byte
- `253..65535`: `0xFD` + 2 bytes LE
- `65536..2^32-1`: `0xFE` + 4 bytes LE

**Source**: `src/consensus/utreexo_accumulator.cpp:214-264`

### 5.2 Internal Node Hash (HashNode)

```
SHA256("DINERO-UTREEXO-NODE-v1" || left_32 || right_32)
```

- Domain tag: 22 ASCII bytes
- Prevents second-preimage attacks (leaf cannot collide with internal node)

**Source**: `src/consensus/utreexo_accumulator.cpp:199-212`

### 5.3 Forest Commitment (getCommitment v2)

```
SHA256(numLeaves_LE64 || slot[0] || slot[1] || ... || slot[63])
```

- numLeaves: 8 bytes little-endian
- 64 fixed root slots, 32 bytes each:
  - If tree at height h exists: root hash
  - If absent: 32 zero bytes
- Slot index = tree height (preserves shape information)
- Total preimage: 8 + 64×32 = **2056 bytes** (always fixed size)
- No special cases: empty forest, single-leaf, full forest all use same formula

**Empty forest** (numLeaves=0): all 64 slots zero → commitment is NOT all-zero bytes
**Single leaf** (numLeaves=1): slot[0] = leaf hash, slots[1..63] = zeros

**Source**: `src/consensus/utreexo_accumulator.cpp:1575-1604`

---

## 6. Difficulty

### Compact Format (nBits)

`target = mantissa × 256^(exponent - 3)` where `nBits = (exponent << 24) | mantissa`

### Schedule

| Height | Phase | Rule |
|--------|-------|------|
| 0-1 | Genesis + Premine | Fixed: 0x1d31ffce |
| 2-200,002 | Bootstrap DAA | Bitcoin-style, 720-block windows |
| 200,003+ | ASERT | Anchored to block 200,002 |

### ASERT Parameters

| Parameter | Value |
|-----------|-------|
| Half-life | 43,200 seconds (12 hours) |
| Target spacing | 120 seconds (2 minutes) |
| Anchor height | 200,002 |
| Anchor bits | 0x1d31ffce |
| POW limit | 0x1d31ffce (50x easier than Bitcoin) |
| Emergency floor | 0x1f00ffff |

Integer arithmetic: 16-bit fixed-point cubic polynomial, no floating-point.

**Source**: `include/consensus/asert_params.h`, `include/consensus/asert_canonical.h`

---

## 7. Monetary Policy

| Constant | Value |
|----------|-------|
| 1 DIN | 100,000,000 una |
| Initial subsidy | 100 DIN/block |
| Halving interval | 1,314,000 blocks (~5 years @ 2 min) |
| Max halvings | 33 |
| Max supply | 265,428,000 DIN |
| Genesis burn | 100 DIN (OP_RETURN, unspendable) |
| Premine (height 1) | 2,627,900 DIN (~1% of max) |
| PoW mineable | 262,800,000 DIN |

### Subsidy Function

```
GetBlockSubsidy(height):
  if height == 0: return 0
  if height == 1: return 262,790,000,000,000 una
  pow_blocks = height - 2
  halvings = pow_blocks / 1,314,000
  if halvings >= 33: return 0
  return 10,000,000,000 >> halvings
```

**Source**: `include/consensus/subsidy.h`

---

## 8. Genesis Block

| Field | Value |
|-------|-------|
| version | 1 |
| prev_block_hash | 0x00...00 (32 zeros) |
| merkle_root | _from coinbase txid_ |
| utreexo_root | _v2 empty forest commitment_ |
| timestamp | 1772496000 (2026-03-03 00:00:00 UTC) |
| difficulty | 0x1d31ffce |
| nonce | _mined_ |
| reserved | 0x00...00 (12 zeros) |

Motto: `Dinero: Real Money For Free People`
Coinbase: 100 DIN to OP_RETURN (unspendable, double commitment of motto)

**Utreexo root**: Empty forest v2 commitment (NOT all zeros).
SHA256(0x0000000000000000 || 64×32_zero_bytes)

---

## 9. Premine Block (Height 1)

| Field | Value |
|-------|-------|
| version | 1 |
| prev_block_hash | genesis hash |
| merkle_root | coinbase txid |
| utreexo_root | v2 commitment of single-leaf forest |
| timestamp | genesis + 600 (1764029400) |
| difficulty | 0x1d31ffce |
| nonce | _mined_ |
| reserved | 0x00...00 (12 zeros) |

Coinbase: 2,627,900 DIN to P2TR address
Address: `din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0`
Derivation: BIP86 m/86'/1447'/0'/0/0
scriptPubKey: `5120ca062bff883f6d1511a72a78a09f3a17cb6734f9ce8543faa66f7e380d58d1b7`

**Utreexo root**: Forest with 1 leaf.
Leaf = HashUTXO(coinbase_txid, 0, 262790000000000, scriptPubKey)
Commitment = SHA256(1_LE64 || leaf_hash || 63×32_zeros)

---

## 10. Address Format

- BIP86 Taproot only (P2TR)
- Derivation: m/86'/1447'/account'/chain/index
- Coin type: 1447 (registered for Dinero)
- HRP: `din`
- Encoding: bech32m, witness version 1
- Key tweak: BIP341 `output_key = internal_key + TaggedHash("TapTweak", internal_key) * G`

---

## 11. Byte Order Convention

| Context | Order | Rule |
|---------|-------|------|
| Header fields | LE | version, timestamp, difficulty, nonce |
| uint256 storage | LE | data[0] = LSB |
| SHA256 output | BE | byte[0] = MSB |
| uint256::GetHex() | BE display | Reverses storage → MSB first |
| Wire/serialization | LE | Same as storage |
| PoW comparison | Via uint256 operator< | Compares MSB first (data[31]) |

**Invariant**: NEVER compare GetHex() strings in consensus. Compare uint256 directly.

---

## 12. Verification

After mining, any implementation MUST reproduce:
1. Genesis header (128 bytes) from fields above → same hash
2. Premine coinbase txid from transaction serialization → same merkle root
3. Premine utreexo_root from HashUTXO + getCommitment → same commitment
4. Premine header (128 bytes) → same hash
5. Both hashes satisfy PoW target (hash <= target from 0x1d31ffce)

**Reproducibility script**: `tools/verify_genesis_bundle.sh` (generated with mining output)

---

## Changelog

- v1 (2026-02-22): Initial freeze. All Phase 1 consensus fixes applied:
  - TaggedHash fixed to BIP340 spec
  - Witness version detection for Taproot
  - HashNode domain separation ("DINERO-UTREEXO-NODE-v1")
  - HashUTXO domain separation + script length prefix ("DINERO-UTXO-LEAF-v1")
  - getCommitment v2 (fixed 64 slots, numLeaves committed)
  - Merkle root consolidated to crypto::CSHA256
  - Double-spend guard in remove()
  - Batch proof and transition proof verification
