# BlockHeader V1 Specification

> **CONSENSUS CRITICAL** - Any deviation from this specification will cause network forks.

## Overview

DineroCoin uses a 128-byte block header (BlockHeader V1), extended from Bitcoin's 80-byte header to include:
- **Utreexo accumulator commitment** (32 bytes)
- **64-bit timestamp** (8 bytes, up from 4)
- **Reserved field** (12 bytes for future extensions)

## Header Layout

| Offset | Size | Field | Endianness | Description |
|--------|------|-------|------------|-------------|
| 0 | 4 | `version` | Little | Block version (currently 1) |
| 4 | 32 | `prev_block_hash` | Little | Hash of previous block header |
| 36 | 32 | `merkle_root` | Little | Merkle root of transactions |
| 68 | 32 | `utreexo_root` | Little | Utreexo accumulator commitment |
| 100 | 8 | `timestamp` | Little | Unix timestamp (64-bit) |
| 108 | 4 | `difficulty` | Little | Compact difficulty target (nBits) |
| 112 | 4 | `nonce` | Little | Mining nonce |
| 116 | 12 | `reserved` | - | Reserved (must be zero) |

**Total: 128 bytes**

## Field Details

### version (4 bytes)
- Current version: `1`
- Little-endian uint32

### prev_block_hash (32 bytes)
- SHA256d hash of previous block's header
- **Internal storage**: Little-endian (LSB at byte 0)
- **Display format** (GetHex): Big-endian (MSB first)
- Genesis block: All zeros

### merkle_root (32 bytes)
- Merkle root of transaction hashes
- Same byte order convention as `prev_block_hash`

### utreexo_root (32 bytes)
- Utreexo accumulator commitment
- **Commits the AFTER-state** of the previous block
- Same byte order convention as `prev_block_hash`
- Genesis (height 0): All zeros (no prior state)
- Premine (height 1): `242c67240afa76c3f6ecd7df56edbdcbb4dfc66eae4059c60917fbe3518a3aeb`

### timestamp (8 bytes)
- Unix timestamp in seconds
- Little-endian uint64
- Genesis: `1772496000` (January 25, 2025 00:00:00 UTC)

### difficulty (4 bytes)
- Compact difficulty target (Bitcoin's nBits format)
- Little-endian uint32
- Mainnet genesis: `0x1d31ffce`
- Testnet genesis: `0x1d00ffff`

### nonce (4 bytes)
- Mining nonce, incremented to find valid hash
- Little-endian uint32

### reserved (12 bytes)
- Reserved for future protocol extensions
- **MUST be all zeros** in current version
- Non-zero reserved bytes → invalid block

## Byte Order Conventions

### Internal Storage (uint256)
- Little-endian: Least significant byte at index 0
- Used in: Memory, serialization, header bytes

### Display Format (GetHex)
- Big-endian: Most significant byte first
- Used in: RPC output, block explorers, logs

### Stratum Protocol
- **Sends**: Big-endian (display format)
- **Miner reverses**: To little-endian for header bytes

```
Daemon (internal LE) → GetHex (BE) → Stratum (BE) → Miner reverses → Header (LE)
```

## Hash Computation

Block hash is computed as:
```
SHA256d(header_bytes) = SHA256(SHA256(128-byte header))
```

The resulting hash is stored/compared in **little-endian** but displayed in **big-endian**.

## Canonical Test Vector

Genesis-style header for cross-implementation testing:

```
Header fields:
  version:      1
  prev_hash:    0000000000000000000000000000000000000000000000000000000000000000
  merkle_root:  c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1
  utreexo_root: 0000000000000000000000000000000000000000000000000000000000000000
  timestamp:    1772496000
  difficulty:   0x1d31ffce
  nonce:        0

Header bytes (128 hex):
  01000000
  0000000000000000000000000000000000000000000000000000000000000000
  0f7d1982fb9c5ae07428dfa0a4acfa6fb540fcc967ea61904c503e248e6c6a41
  0000000000000000000000000000000000000000000000000000000000000000
  80f1246900000000
  ceff311d
  00000000
  000000000000000000000000

Expected hash: [computed by reference implementation]
```

## Validation Rules

1. **Size**: Header MUST be exactly 128 bytes
2. **Version**: MUST be 1 (current)
3. **Reserved**: MUST be all zeros
4. **Timestamp**: MUST be > previous block's timestamp
5. **Difficulty**: MUST match expected difficulty (ASERT calculation)
6. **Hash**: MUST be ≤ difficulty target
7. **Utreexo**: MUST match computed accumulator state

## Implementation References

- `src/primitives/block.h` - BlockHeader struct
- `src/mining/header_layout.h` - Offset constants
- `src/common/uint256.h` - uint256 byte order
- `tests/mining/test_dinero_mining_pipeline.cpp` - Conformance tests

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1 | 2025-01 | Initial 128-byte header with Utreexo |

---

**WARNING**: Modifying this specification requires a hard fork. All implementations
(daemon, miners, explorers, wallets) MUST agree on these exact bytes.
