# DINERO-UTREEXO-SPEC: Domain-Separated Leaf and Node Hashing

```
Title:   Domain-Separated UTXO Leaf and Node Hashing for Utreexo
Status:  Consensus-Critical (Active from Genesis)
Version: 1
Created: 2026-02-05
Updated: 2026-04-23  (corrected §3.2 varint; §3.3 domain-tagged node
                      hash; §3.5 regenerated against current C++;
                      added §3.6 for node hash + commitment vectors)
```

## 1. Abstract

This document specifies the domain-separated hash algorithms used by
DineroCoin's Utreexo accumulator:

- **Leaf hash** for each UTXO added to the forest.
- **Node hash** for each internal Merkle-tree node.
- **Commitment** that folds the whole forest (up to 64 roots) into the
  single 32-byte `utreexo_root` stored at block-header offset `0x44`.

All three use single-round SHA-256 with fixed ASCII domain tags.

## 2. Motivation

A bare `SHA256(txid || vout || amount || scriptPubKey)` leaf hash
shares the same construction as internal Merkle nodes
(`SHA256(left || right)`). If an attacker can craft a 64-byte UTXO
preimage that collides with a valid `left || right` pair, they can
forge inclusion proofs.

Domain separation eliminates this class of attack by prepending a
fixed tag that each category never shares with another.

## 3. Specification

### 3.1 Domain Tags

| Tag | ASCII | Length |
|-----|-------|--------|
| Leaf | `DINERO-UTXO-LEAF-v1`    | 19 bytes |
| Node | `DINERO-UTREEXO-NODE-v1` | 22 bytes |

Tags are raw ASCII with **no null terminator**.

### 3.2 Leaf Hash Algorithm

```
LeafHash = SHA256(
    TAG
    || txid
    || vout
    || amount
    || CompactSize(len(scriptPubKey))
    || scriptPubKey
)
```

| Field                 | Size     | Encoding              |
|-----------------------|----------|-----------------------|
| TAG                   | 19 bytes | ASCII literal         |
| txid                  | 32 bytes | raw bytes             |
| vout                  | 4 bytes  | little-endian         |
| amount                | 8 bytes  | little-endian (una)   |
| scriptPubKey length   | 1-9 bytes| Bitcoin CompactSize   |
| scriptPubKey          | variable | raw bytes             |

CompactSize length prefix semantics (same as Bitcoin):

| `len` range          | Encoding                |
|----------------------|-------------------------|
| `< 0xFD`             | `1 byte: len`           |
| `0xFD..=0xFFFF`      | `0xFD + u16 LE`         |
| `0x10000..=0xFFFFFFFF` | `0xFE + u32 LE`       |
| `>= 2^32`            | `0xFF + u64 LE`         |

Minimum preimage size: **64 bytes** (empty `scriptPubKey`, one-byte
varint = `0x00`).

### 3.3 Internal Node Hash

Internal nodes are **also domain-tagged**:

```
NodeHash = SHA256( TAG_NODE || left_child || right_child )
```

| Field        | Size     | Encoding       |
|--------------|----------|----------------|
| TAG_NODE     | 22 bytes | ASCII literal  |
| left_child   | 32 bytes | raw bytes      |
| right_child  | 32 bytes | raw bytes      |

Fixed preimage size: **86 bytes**. Because leaf and node tags differ
in length and content, no leaf preimage can ever equal a node
preimage — the 64-byte-collision class is closed by construction,
independent of script length.

### 3.4 Activation

Both domain-separated hashes are active **from genesis** (height 0).
There is no legacy mode and no activation height. All nodes must use
the tagged hashes from the first block.

### 3.5 Golden Leaf Vectors

All hashes below were produced by the current C++ implementation at
`src/consensus/utreexo_accumulator.cpp:216`. Any implementation that
reproduces these byte-for-byte is compatible with dinerod's consensus
code.

#### Vector 1: Standard UTXO

```
txid:          abababababababababababababababababababababababababababababababab
vout:          0
amount:        50000000 (0.5 DIN)
scriptPubKey:  51 (OP_1, 1 byte)

preimage (hex): 44494e45524f2d5554584f2d4c4541462d7631
                abababababababababababababababababababababababababababababababab
                00000000
                80f0fa0200000000
                01
                51
preimage size:  65 bytes

LeafHash: 7296f90cc934276efe66d7dcd90a0913cbf7683ef0e3d520b6d70afb437e0603
```

#### Vector 2: Zero Inputs (empty scriptPubKey)

```
txid:          0000000000000000000000000000000000000000000000000000000000000000
vout:          0
amount:        0
scriptPubKey:  (empty; CompactSize varint = 00)

preimage size:  64 bytes

LeafHash: 9cf25a8b513f06a0896fad9d16aaf80b0c2b587c0710bf1ed0ec73c00ceb23b0
```

#### Vector 3: Maximum Values

```
txid:          ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
vout:          4294967295 (0xffffffff)
amount:        18446744073709551615 (0xffffffffffffffff)
scriptPubKey:  0014cccccccccccccccccccccccccccccccccccccccc (22 bytes;
               CompactSize varint = 16 (0x16))

preimage size:  86 bytes

LeafHash: 5b3a94af3b1b99095f8c430251dc3a90f367ca1b66194814f5d9adf120d3d6cc
```

#### Vector 4: Domain Separation Proof (leaf)

Same inputs as Vector 1, computed **without** the 19-byte leaf tag:

```
SHA256(       txid || vout || amount || varint(1) || scriptPubKey )  [NO TAG]
= 14890b1801c6ce17a0b1de1db2ceb3e7992b91adcf1e3ee8f06a875d9d2c5a0c

SHA256( TAG || txid || vout || amount || varint(1) || scriptPubKey )  [WITH TAG]
= 7296f90cc934276efe66d7dcd90a0913cbf7683ef0e3d520b6d70afb437e0603
```

These MUST differ. Any implementation producing the untagged hash is
non-compliant.

### 3.6 Node Hash + Commitment Vectors

Node hash (leaves `0x11 × 32` and `0x22 × 32`):

```
left:  1111111111111111111111111111111111111111111111111111111111111111
right: 2222222222222222222222222222222222222222222222222222222222222222

preimage = TAG_NODE || left || right
         = 44494e45524f2d5554524545584f2d4e4f44452d7631
           1111…
           2222…
preimage size:  86 bytes

NodeHash = SHA256(preimage)
         ≠ SHA256(left || right)           (untagged — MUST differ)
```

Commitment (produced by `UtreexoForest::getCommitment()` at
`src/consensus/utreexo_accumulator.cpp:1845`):

```
commitment = SHA256(
    num_leaves (u64 LE)
    || slot[0] || slot[1] || … || slot[63]
)
```

where each `slot[h]` is 32 bytes — the forest root at height `h` if
bit `h` of `num_leaves` is set, otherwise 32 zero bytes. Preimage
size is always **2056 bytes** regardless of populated roots.

**Live-RPC golden** (captured 2026-04-23 against the Mac regtest daemon):

```
num_leaves = 15939
num_roots  = 8
roots      = [
  "6ae5efa9adfd812cecc72f7c837bc0bb69d1fbe5b266b18106189aec45b78efc",
  "6afe3b3f3b3bc35ce500a41c0d57a6278f21fea45aa977632f62a8a8a5f0fbcc",
  "669d8e491ddad3394ed1aaa59585896534c736ee56528a040759b1bcdaf9759e",
  "4c114937d8fbdb494e4276434f30769bbd2ae1b14c7848ccaead6d45c5f7f9ca",
  "8327ae19ceb62f7abf3389cf6960a35d61164b11ea8fe0ef72cc03c393e57b76",
  "62cee458984d6b31b478095a57f32de61c0982beb2a21c58a7df8f96095c8805",
  "5dee4fafac4c04ac08395e9cc83c6f4853ac6b460f5da2675ea4928cd9cb671f",
  "c2742beec976b89d4e8bbb5d7e8ae87677d2736d35c8dea11807c603eb31ee2d",
]

expected commitment = cb8592403b46beee369754ed54f642ffab310ae0df4d85661e681d339ea12ce4
```

The `num_leaves` binary representation (`0b11111001000011`) determines
which slots are populated: bits 0, 1, 6, 9, 10, 11, 12, 13. Any
implementation that populates slots in that order and hashes the
2056-byte preimage must produce the same 32-byte commitment.

## 4. Implementation Reference

### C++ (Authority)

```cpp
// src/consensus/utreexo_accumulator.cpp:216
UtreexoHash HashUTXO(const uint256& txid, uint32_t vout,
                     uint64_t amount,
                     const std::vector<uint8_t>& scriptPubKey) {
    static const char* DOMAIN_TAG = "DINERO-UTXO-LEAF-v1"; // 19 bytes
    std::vector<uint8_t> data;
    data.insert(data.end(), DOMAIN_TAG, DOMAIN_TAG + 19);
    data.insert(data.end(), txid.data, txid.data + 32);
    // vout (u32 LE)
    for (int i = 0; i < 4; i++) data.push_back((vout >> (8*i)) & 0xFF);
    // amount (u64 LE)
    for (int i = 0; i < 8; i++) data.push_back((amount >> (8*i)) & 0xFF);
    // scriptPubKey length (Bitcoin CompactSize varint)
    uint64_t n = scriptPubKey.size();
    if (n < 0xFD) {
        data.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        data.push_back(0xFD);
        data.push_back(n & 0xFF); data.push_back((n >> 8) & 0xFF);
    } else {
        data.push_back(0xFE);
        for (int i = 0; i < 4; i++) data.push_back((n >> (8*i)) & 0xFF);
    }
    data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());
    return SHA256_Hash(data);
}
```

```cpp
// src/consensus/utreexo_accumulator.cpp:201
UtreexoHash HashNode(const UtreexoHash& left, const UtreexoHash& right) {
    static const char* DOMAIN_TAG = "DINERO-UTREEXO-NODE-v1"; // 22 bytes
    std::vector<uint8_t> combined;
    combined.reserve(22 + 64);
    combined.insert(combined.end(), DOMAIN_TAG, DOMAIN_TAG + 22);
    combined.insert(combined.end(), left.begin(), left.end());
    combined.insert(combined.end(), right.begin(), right.end());
    return SHA256_Hash(combined);
}
```

### Rust (Tier-3 Verification)

A standalone pure-Rust port with byte-for-byte-verified golden
vectors against the current C++ lives at
[`github.com/Trucker2827/dinero-sv2`](https://github.com/Trucker2827/dinero-sv2)
under `crates/dinero-sv2-jd/src/utreexo.rs`. Snippet:

```rust
pub fn leaf_hash(
    txid: &[u8; 32],
    vout: u32,
    amount: u64,
    script_pubkey: &[u8],
) -> [u8; 32] {
    const TAG: &[u8] = b"DINERO-UTXO-LEAF-v1"; // 19 bytes
    let mut buf = Vec::with_capacity(TAG.len() + 32 + 4 + 8 + 9 + script_pubkey.len());
    buf.extend_from_slice(TAG);
    buf.extend_from_slice(txid);
    buf.extend_from_slice(&vout.to_le_bytes());
    buf.extend_from_slice(&amount.to_le_bytes());
    write_compact_size(&mut buf, script_pubkey.len() as u64);
    buf.extend_from_slice(script_pubkey);
    sha256(&buf)
}

pub fn node_hash(left: &[u8; 32], right: &[u8; 32]) -> [u8; 32] {
    const TAG: &[u8] = b"DINERO-UTREEXO-NODE-v1"; // 22 bytes
    let mut buf = [0u8; 22 + 32 + 32];
    buf[..22].copy_from_slice(TAG);
    buf[22..54].copy_from_slice(left);
    buf[54..].copy_from_slice(right);
    sha256(&buf)
}
```

That crate's integration test `tests/utreexo_live.rs` runs the
2056-byte commitment formula against a fixture harvested from a live
`dinero-cli getutreexoroots` + `getutreexocommitment` and asserts
byte-for-byte agreement with the daemon.

## 5. Security Properties

1. **Leaf-Node Separation**: No valid UTXO leaf hash can collide with
   an internal node hash, because both preimages start with
   different-length domain tags whose bytes disagree in the first 19
   characters.

2. **Cross-Protocol Isolation**: The `DINERO-` prefix ensures Dinero
   leaf and node hashes cannot collide with those of other Utreexo
   implementations (e.g., Bitcoin's proposed Utreexo).

3. **Version Extensibility**: The `-v1` suffix on each tag allows
   future hash algorithm changes via a new tag (e.g.,
   `DINERO-UTXO-LEAF-v2`) without ambiguity at forks.

4. **Shape Commitment**: The 64-slot commitment preimage includes
   every height slot (zero-filled when empty), so the commitment
   binds not just the roots but the forest's exact shape — two
   different forests with the same non-empty roots but different
   `num_leaves` produce different commitments.

## 6. References

- Dryja, T. (2019). "Utreexo: A dynamic hash-based accumulator
  optimized for the Bitcoin UTXO set"
- DIN-UTREEXO-SPEC.md (predecessor, now superseded for leaf and
  node hashing)
- `dinero-sv2` repository — pure-Rust port with cross-verification
  harness
