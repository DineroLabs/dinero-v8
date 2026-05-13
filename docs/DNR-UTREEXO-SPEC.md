# Dinero Utreexo Specification v1.0

```
  DIN-UTREEXO-SPEC
  Title: Utreexo UTXO Accumulator for Dinero
  Author: DineroCoin Development Team
  Status: Active
  Type: Consensus
  Created: 2025-01-XX
```

## Abstract

This document specifies the Utreexo cryptographic accumulator implementation for DineroCoin. Utreexo enables compact UTXO set commitments in block headers, allowing nodes to verify the entire UTXO state with O(log n) proof sizes instead of maintaining the full UTXO database.

## Motivation

Traditional Bitcoin-style nodes must store the entire UTXO set (~5GB for Bitcoin) to validate transactions. Utreexo reduces this requirement by:

1. Committing to the UTXO set via a Merkle forest in each block header
2. Allowing pruned nodes to verify transactions using compact inclusion proofs
3. Enabling instant sync via UTXO snapshots with cryptographic verification

## Specification

### 1. Block Header Format

Dinero uses **128-byte block headers** (vs Bitcoin's 80 bytes):

| Field           | Offset | Size | End Byte | Notes |
|-----------------|--------|------|----------|-------|
| version         | 0      | 4    | 3        | little-endian uint32 |
| prev_block_hash | 4      | 32   | 35       | 32-byte hash |
| merkle_root     | 36     | 32   | 67       | 32-byte hash |
| utreexo_root    | 68     | 32   | 99       | 32-byte hash |
| timestamp       | 100    | 8    | 107      | little-endian uint64 |
| difficulty      | 108    | 4    | 111      | compact bits (uint32) |
| nonce           | 112    | 4    | 115      | little-endian uint32 |
| reserved        | 116    | 12   | 127      | MUST be zero |

### 2. Accumulator Model: AFTER-State

**Critical Design Decision**: Block N's header commits to the UTXO state **AFTER** applying Block N's transactions.

```
┌─────────────────────────────────────────────────────────────────┐
│                    UTREEXO STATE TRANSITIONS                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Genesis Block (Height 0)                                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ Header.utreexoCommitment = 0x00...00 (all zeros)         │   │
│  │ Chainstate AFTER genesis = [genesis_utxo_1, genesis_utxo_2]  │
│  └──────────────────────────────────────────────────────────┘   │
│                           │                                     │
│                           ▼                                     │
│  Block 1 (Height 1)                                             │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 1. Start with chainstate from genesis (2 leaves)         │   │
│  │ 2. Process Block 1 transactions:                         │   │
│  │    - Delete spent UTXOs (if any)                         │   │
│  │    - Add new outputs (coinbase + tx outputs)             │   │
│  │ 3. Header.utreexoCommitment = commitment AFTER step 2    │   │
│  └──────────────────────────────────────────────────────────┘   │
│                           │                                     │
│                           ▼                                     │
│  Block N (Height N)                                             │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ Header.utreexoCommitment = State AFTER Block N applied   │   │
│  │                                                          │   │
│  │ Verification:                                            │   │
│  │   simulated = chainstate.clone()                         │   │
│  │   simulated.applyBlock(BlockN)                           │   │
│  │   assert(BlockN.header.utreexoCommitment                 │   │
│  │          == simulated.getCommitment())                   │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3. UTXO Leaf Hash Algorithm

> **NOTE**: Leaf hashing now uses domain separation. The canonical specification
> is **DINERO-UTREEXO-SPEC.md**. The algorithm below is updated to match.

Each UTXO is represented as a 32-byte leaf in the Merkle forest:

```cpp
Hash256 HashUTXO(string txid, uint32_t vout, uint64_t amount, vector<uint8_t> scriptPubKey) {
    vector<uint8_t> preimage;

    // 0. Domain separation tag (19 bytes ASCII, no null terminator)
    preimage.append("DINERO-UTXO-LEAF-v1");

    // 1. txid (32 bytes, as raw bytes)
    preimage.append(txid);

    // 2. vout (4 bytes, little-endian)
    preimage.append_le32(vout);

    // 3. amount (8 bytes, little-endian, in una = 1e-8 DIN)
    preimage.append_le64(amount);

    // 4. scriptPubKey (variable length)
    preimage.append(scriptPubKey);

    return SHA256(preimage);
}
```

**Example:**
```
txid:         "abcd1234..." (64 hex chars = 32 bytes)
vout:         0
amount:       10000000000 (100 DIN = 100 * 1e8 una)
scriptPubKey: [0x00, 0x14, ...] (witness v0 keyhash)

leaf_hash = SHA256(txid_bytes || vout_le32 || amount_le64 || scriptPubKey)
```

### 4. Merkle Forest Structure

Utreexo uses a **Merkle forest** (multiple perfect binary trees) rather than a single Merkle tree:

```
Example: 5 leaves = 1 tree of 4 + 1 tree of 1

       Root₀              Root₁
      /    \                │
    H₀₁    H₂₃            Leaf₄
   /  \    /  \
  L₀  L₁  L₂  L₃

Commitment = SHA256(Root₀ || Root₁)
```

**Forest Properties:**
- Number of trees = popcount(num_leaves) (number of 1-bits)
- Tree sizes are powers of 2
- Trees are ordered by size (largest first)

### 5. Commitment Calculation

The single 32-byte commitment is computed from forest roots:

```cpp
Hash256 getCommitment() {
    if (roots_.empty()) {
        return Hash256{0};  // All zeros for empty forest
    }

    // Concatenate all non-empty roots
    vector<uint8_t> data;
    for (const auto& root : roots_) {
        if (!isZero(root)) {
            data.append(root);
        }
    }

    if (data.empty()) {
        return Hash256{0};
    }

    return SHA256(data);
}
```

### 6. Block Processing Algorithm

#### 6.1 Mining (Block Creation)

```cpp
Block createBlock(transactions, coinbase_address) {
    Block block;

    // 1. Build standard header fields
    block.header.prevHash = chainstate.getTipHash();
    block.header.merkleRoot = computeMerkleRoot(transactions);
    block.header.timestamp = now();
    block.header.bits = getNextTarget();

    // 2. Collect outputs that will be ADDED to accumulator
    vector<Output> new_outputs;
    for (tx in transactions) {
        for (output in tx.outputs) {
            if (output.amount > 0) {
                new_outputs.push_back({tx.txid, output.index,
                                       output.amount, output.scriptPubKey});
            }
        }
    }

    // 3. Compute AFTER-state commitment
    // Clone current forest, apply adds, get commitment
    block.header.utreexoCommitment =
        chainstate.computeCommitmentAfterAdds(new_outputs);

    // 4. Mine (find valid nonce)
    while (!meetsTarget(block.header)) {
        block.header.nonce++;
    }

    return block;
}
```

#### 6.2 Block Acceptance (Validation)

```cpp
bool acceptBlock(Block block) {
    // 1. Validate standard consensus rules
    if (!validatePoW(block)) return false;
    if (!validateMerkleRoot(block)) return false;
    if (!validateTransactions(block)) return false;

    // 2. Simulate accumulator state AFTER this block
    UtreexoForest simulated = chainstate.utreexo.clone();

    // 2a. Delete spent UTXOs
    for (tx in block.transactions) {
        for (input in tx.inputs) {
            if (!input.isCoinbase()) {
                Hash256 leaf = HashUTXO(input.prevTxid, input.prevVout,
                                        input.amount, input.scriptPubKey);
                simulated.delete(leaf);
            }
        }
    }

    // 2b. Add new outputs
    for (tx in block.transactions) {
        for (output in tx.outputs) {
            if (output.amount > 0) {
                Hash256 leaf = HashUTXO(tx.txid, output.index,
                                        output.amount, output.scriptPubKey);
                simulated.add(leaf);
            }
        }
    }

    // 3. Verify commitment matches
    if (block.header.utreexoCommitment != simulated.getCommitment()) {
        return false;  // REJECT: Utreexo mismatch
    }

    // 4. Apply to real chainstate
    chainstate.utreexo = simulated;
    chainstate.tip = block.hash;

    return true;
}
```

### 7. Genesis Block

The genesis block has special handling:

```cpp
// Genesis header commitment is all zeros
genesis.header.utreexoCommitment = Hash256{0};

// Genesis coinbase creates initial UTXOs
// DineroCoin genesis has 2 outputs:
//   vout=1: 100 DIN
//   vout=2: 100 DIN

// After genesis is processed, chainstate has 2 leaves
chainstate.utreexo.add(HashUTXO(genesis_txid, 1, 100_DIN, script1));
chainstate.utreexo.add(HashUTXO(genesis_txid, 2, 100_DIN, script2));
```

### 8. Reorg Handling

During a chain reorganization:

```cpp
void handleReorg(oldTip, newTip) {
    // 1. Find common ancestor
    Block ancestor = findCommonAncestor(oldTip, newTip);

    // 2. Disconnect blocks from old chain
    for (block in oldTip.pathTo(ancestor).reverse()) {
        // Undo: remove added outputs, restore deleted inputs
        for (tx in block.transactions.reverse()) {
            for (output in tx.outputs) {
                utreexo.delete(HashUTXO(tx, output));
            }
            for (input in tx.inputs) {
                if (!input.isCoinbase()) {
                    utreexo.add(HashUTXO(input.prevTx, input));
                }
            }
        }
    }

    // 3. Connect blocks from new chain
    for (block in ancestor.pathTo(newTip)) {
        acceptBlock(block);
    }
}
```

### 9. Persistence

The accumulator state is rebuilt on node restart:

```
┌─────────────────────────────────────────────────────────────┐
│                    NODE RESTART SEQUENCE                    │
├─────────────────────────────────────────────────────────────┤
│ 1. Load chain index from disk                               │
│ 2. Load UTXO database from disk                             │
│ 3. Rebuild Utreexo accumulator:                             │
│    - Iterate through all UTXOs                              │
│    - Add each UTXO leaf to fresh forest                     │
│    - Note: Order is NOT deterministic (DB iteration order)  │
│ 4. Verify tip block header matches rebuilt commitment       │
│    - MAY NOT MATCH due to order difference                  │
│    - This is expected behavior                              │
│ 5. Node is ready for new blocks                             │
└─────────────────────────────────────────────────────────────┘
```

**Important**: The rebuilt accumulator may have a different commitment than the stored tip header due to leaf insertion order. This does NOT indicate corruption. The live accumulator (built incrementally during block acceptance) will always match headers.

### 10. RPC Interface

#### blockchain.getutreexocommitment

Returns the current accumulator commitment.

**Request:**
```json
{"jsonrpc":"2.0","method":"blockchain.getutreexocommitment","params":[],"id":1}
```

**Response:**
```json
{
  "result": {
    "commitment": "4bafb89cee3ba8cb...",
    "num_leaves": 105,
    "num_roots": 3
  }
}
```

#### blockchain.getutreexostats

Returns accumulator statistics.

**Response:**
```json
{
  "result": {
    "num_leaves": 105,
    "num_roots": 3,
    "total_size": 4096,
    "avg_proof_size": 208
  }
}
```

#### blockchain.getutxoproof

Generates an inclusion proof for a UTXO.

**Request:**
```json
{"jsonrpc":"2.0","method":"blockchain.getutxoproof","params":["txid",0],"id":1}
```

**Response:**
```json
{
  "result": {
    "txid": "abcd1234...",
    "vout": 0,
    "proof": {
      "siblings": ["hash1", "hash2", "hash3", "hash4", "hash5", "hash6"],
      "position": 42,
      "num_leaves": 105
    },
    "proof_size": 208
  }
}
```

#### blockchain.getutreexoroots

Returns all forest roots.

**Response:**
```json
{
  "result": {
    "num_leaves": 105,
    "num_roots": 3,
    "roots": [
      "root0_hash...",
      "root1_hash...",
      "root2_hash..."
    ]
  }
}
```

### 11. Proof Size Analysis

For a forest with N leaves:
- Number of siblings in proof: O(log₂ N)
- Proof size: 32 * ceil(log₂ N) bytes

| UTXOs | Proof Siblings | Proof Size |
|-------|----------------|------------|
| 100   | 7              | 224 bytes  |
| 1,000 | 10             | 320 bytes  |
| 10,000 | 14            | 448 bytes  |
| 100,000 | 17           | 544 bytes  |
| 1,000,000 | 20         | 640 bytes  |

### 12. Security Considerations

1. **Collision Resistance**: The leaf hash function must be collision-resistant. Any collision would allow double-spending.

2. **Order Dependence**: The accumulator is order-dependent. Nodes must process UTXOs in the same order to achieve consensus.

3. **Proof Binding**: Inclusion proofs are bound to a specific commitment. Proofs from stale commitments will fail verification.

4. **Pruning Safety**: Nodes using only proofs (no local UTXO DB) depend on honest proof providers. SPV-level security.

## Implementation Notes

### Source Files

| File | Purpose |
|------|---------|
| `include/consensus/utreexo_accumulator.h` | UtreexoForest class definition |
| `src/consensus/utreexo_accumulator.cpp` | Forest add/delete/prove operations |
| `src/consensus/global_utxo_set.cpp` | HashUTXO, computeCommitmentAfterAdds |
| `src/rpc/methods_utreexo.cpp` | RPC handlers |
| `src/daemon/block_acceptor.cpp` | Block validation with Utreexo |
| `src/rpc/methods_mining_context.cpp` | Mining with AFTER-state commitment |

### Key Variables

- `utreexo_forest_` in `GlobalUTXOSet` - The live accumulator
- `roots_` in `UtreexoForest` - Vector of forest roots
- `numLeaves_` in `UtreexoForest` - Total leaf count

## Test Vectors

### Test 1: Empty Forest
```
num_leaves = 0
commitment = 0x0000000000000000000000000000000000000000000000000000000000000000
```

### Test 2: Single Leaf
```
leaf = SHA256("test_utxo_data")
num_leaves = 1
roots = [leaf]
commitment = SHA256(leaf)
```

### Test 3: Two Leaves
```
leaf0 = SHA256("utxo_0")
leaf1 = SHA256("utxo_1")
num_leaves = 2
roots = [SHA256(leaf0 || leaf1)]
commitment = SHA256(roots[0])
```

## References

1. Utreexo: A dynamic hash-based accumulator optimized for the Bitcoin UTXO set (Dryja, 2019)
2. Bitcoin Improvement Proposals (BIPs)
3. DineroCoin Source Code

## Copyright

This document is placed in the public domain.
