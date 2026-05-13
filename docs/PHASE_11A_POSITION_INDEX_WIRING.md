# Phase 11a: UTXO Position Index - Implementation Guide

**Status**: ✅ Infrastructure Complete - Ready for Wiring
**Date**: 2026-01-17

---

## What Was Built

### Global UTXO Position Index ✅

**Location**: `src/indexing/utxo_position_index.{h,cpp}`

**Purpose**: Maps ALL UTXOs (not wallet-specific) to their Utreexo accumulator positions

**API**:
```cpp
namespace dinero::indexing {

class UTXOPositionIndex {
public:
    // Add position when UTXO is created
    void AddPosition(const TxId& txid, uint32_t vout, uint64_t position);

    // Remove position when UTXO is spent (returns position for undo)
    std::optional<uint64_t> RemovePosition(const TxId& txid, uint32_t vout);

    // Query position for proof generation (PRIMARY API)
    std::optional<uint64_t> GetPosition(const TxId& txid, uint32_t vout) const;

    // Statistics
    size_t GetPositionCount() const;
    bool HasPosition(const TxId& txid, uint32_t vout) const;
    void Clear();
};

}
```

**Properties**:
- ✅ Tracks ALL UTXOs (not just wallet-owned)
- ✅ Non-consensus (rebuildable)
- ✅ Indexing-layer component (like txindex)
- ✅ Thread-safe for concurrent queries
- ✅ O(1) lookup performance

---

## What Needs to Be Wired

### Step 1: Add UTXOPositionIndex to ChainstateService

**File**: `include/daemon/services/chainstate_service.h`

Add member:
```cpp
private:
    // Phase 11a: Global UTXO → Utreexo position index
    std::unique_ptr<indexing::UTXOPositionIndex> utxo_position_index_;
```

Add accessor:
```cpp
public:
    // Phase 11a: Get UTXO position index for proof generation
    indexing::UTXOPositionIndex* GetUTXOPositionIndex() { return utxo_position_index_.get(); }
    const indexing::UTXOPositionIndex* GetUTXOPositionIndex() const { return utxo_position_index_.get(); }
```

**File**: `src/daemon/services/chainstate_service.cpp`

In constructor:
```cpp
ChainstateService::ChainstateService(...)
    : /* existing members */
    , utxo_position_index_(std::make_unique<indexing::UTXOPositionIndex>())
{
    // ...
}
```

---

### Step 2: Find the ConnectBlock Hook Point

**Search for**:
- The function that processes a new block
- Where UtreexoForest is updated
- Where new UTXOs are added

**Likely locations** (check in this order):
1. `src/consensus/block_validation.cpp::ConnectBlock()`
2. `src/daemon/services/chainstate_service.cpp::ApplyBlock()`
3. `src/consensus/block_acceptor.cpp::AcceptBlock()`

**What to look for**:
```cpp
// Where forest leaves are added:
uint64_t position = forest->add(leafHash);

// OR where block outputs are processed:
for (const auto& tx : block.vtx) {
    for (const auto& output : tx.vout) {
        // Add to UTXO set
    }
}
```

---

### Step 3: Capture Positions from UtreexoForest

**Critical Rule**: UTXOPositionIndex must NOT compute positions. Only the forest knows positions.

**Required API** (check if this exists in UtreexoForest):

**Option A (Preferred)**:
```cpp
struct AddedLeaf {
    TxId txid;
    uint32_t vout;
    uint64_t position;
};

std::vector<AddedLeaf> UtreexoForest::ApplyBlockAndReturnNewLeaves(const Block& block);
```

**Option B (Acceptable)**:
```cpp
// If UtreexoForest::add() returns position:
for (each new UTXO) {
    uint64_t position = forest->add(utxo_hash);
    // Now we have the position
}
```

**What to check**:
```bash
# Search for forest add methods
grep -n "uint64_t add\|add.*return.*position" include/consensus/utreexo_accumulator.h
```

Current API from search results:
```cpp
uint64_t add(const UtreexoHash& leafHash);  // Returns position ✅
```

This is **Option B** - the `add()` method already returns the position!

---

### Step 4: Wire Position Tracking in ConnectBlock

**Pseudocode**:
```cpp
// In ConnectBlock or equivalent:

auto* position_index = chainstate->GetUTXOPositionIndex();

for (const auto& tx : block.vtx) {
    // Skip coinbase inputs
    if (!tx.IsCoinBase()) {
        // 1. Handle inputs (spent UTXOs)
        for (const auto& input : tx.vin) {
            TxId prev_txid = /* extract from input */;
            uint32_t prev_vout = input.prevout.n;

            // Remove position (returns old position for undo)
            auto old_pos = position_index->RemovePosition(prev_txid, prev_vout);

            // Save to undo log
            block_undo.removed_positions.push_back({prev_txid, prev_vout, *old_pos});
        }
    }

    // 2. Handle outputs (created UTXOs)
    TxId txid = tx.GetTxId();
    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
        // Compute UTXO hash for Utreexo
        UtreexoHash leaf_hash = ComputeUTXOHash(txid, vout, tx.vout[vout]);

        // Add to forest (FOREST RETURNS POSITION)
        uint64_t position = forest->add(leaf_hash);

        // Record position in index
        position_index->AddPosition(txid, vout, position);

        // Save to undo log
        block_undo.added_positions.push_back({txid, (uint32_t)vout, position});
    }
}
```

**Key Points**:
- ✅ Position comes from `forest->add()` return value
- ✅ Removed positions saved to undo log BEFORE removal
- ✅ Added positions saved to undo log AFTER addition
- ✅ Both coinbase and regular outputs get positions

---

### Step 5: Wire Position Restoration in DisconnectBlock

**Pseudocode**:
```cpp
// In DisconnectBlock or equivalent:

auto* position_index = chainstate->GetUTXOPositionIndex();

// Restore in REVERSE order

// 1. Remove UTXOs that were added in this block
for (const auto& entry : reverse(block_undo.added_positions)) {
    position_index->RemovePosition(entry.txid, entry.vout);
}

// 2. Restore UTXOs that were removed in this block
for (const auto& entry : reverse(block_undo.removed_positions)) {
    position_index->AddPosition(entry.txid, entry.vout, entry.position);
}
```

**Important**:
- Process in REVERSE order
- No recomputation needed (positions from undo log)
- Perfect symmetry with ConnectBlock

---

### Step 6: Undo Data Structure

**File**: Create `include/indexing/position_undo.h` (or add to existing undo header)

```cpp
#pragma once

#include "primitives/hash_domains.h"
#include <vector>
#include <cstdint>

namespace dinero {
namespace indexing {

struct PositionUndoEntry {
    TxId txid;
    uint32_t vout;
    uint64_t position;
};

struct BlockPositionUndo {
    uint32_t height;
    std::vector<PositionUndoEntry> removed_positions;
    std::vector<PositionUndoEntry> added_positions;
};

} // namespace indexing
} // namespace dinero
```

**Storage**:
- Option A: Store alongside existing BlockUndo (separate field)
- Option B: Store in separate database (like txindex undo)

Recommendation: **Option A** (simpler, already have undo infrastructure)

---

## Proof of Concept: Trivial Proof Generation

Once wired, this becomes trivial:

**File**: `src/consensus/global_utxo_set_impl.cpp`

```cpp
std::optional<UtreexoProof>
GlobalUTXOSetImpl::GenerateProof(const uint256& txid, uint32_t vout) const {
    // Get position index from chainstate
    auto* position_index = chainstate_->GetUTXOPositionIndex();
    if (!position_index) {
        return std::nullopt;
    }

    // O(1) position lookup
    TxId tx_id = TxId::FromUint256(txid);
    auto position = position_index->GetPosition(tx_id, vout);
    if (!position) {
        return std::nullopt;  // UTXO not tracked
    }

    // Generate proof from forest
    return forest_->prove(*position);
}
```

**That's it. No recomputation. No traversal. O(1).**

---

## Batch RPC Example

Once wired, batch RPCs are trivial:

```cpp
Json rpc_getutxoproofs_batch(const ExecutionContext& ctx, const Json& params) {
    auto* position_index = chainstate->GetUTXOPositionIndex();
    auto* forest = chainstate->utreexoForest();

    Json results;
    for (const auto& utxo : utxos_param) {
        TxId txid = /* parse */;
        uint32_t vout = /* parse */;

        // O(1) lookup
        auto position = position_index->GetPosition(txid, vout);
        if (!position) {
            results.append({txid, vout, "error": "not found"});
            continue;
        }

        // O(log n) proof generation
        auto proof = forest->prove(*position);
        if (!proof) {
            results.append({txid, vout, "error": "proof failed"});
            continue;
        }

        results.append({txid, vout, "proof": serialize(*proof)});
    }

    return results;
}
```

**Performance**: 1000+ proofs/second achievable

---

## Rebuild Procedure (Future Phase)

**Not required for initial wiring**, but document the design:

```cpp
bool UTXOPositionIndex::Rebuild(const ChainstateService& chainstate) {
    Clear();

    // 1. Iterate all UTXOs in ChainDB
    auto* chaindb = chainstate.GetChainDB();
    auto all_utxos = chaindb->GetAllUTXOs();

    // 2. For each UTXO, query its position from forest
    auto* forest = chainstate.utreexoForest();

    for (const auto& [txid, vout, coin] : all_utxos) {
        // Compute UTXO hash
        UtreexoHash hash = ComputeUTXOHash(txid, vout, coin);

        // Find position in forest (requires forest search API)
        auto position = forest->FindLeafPosition(hash);
        if (position) {
            AddPosition(txid, vout, *position);
        }
    }

    return true;
}
```

**Requires**:
- `ChainDB::GetAllUTXOs()` iterator
- `UtreexoForest::FindLeafPosition(hash)` search API

**Use cases**:
- Initial index creation
- Recovery from corruption
- Snapshot imports

---

## Testing Plan

### Unit Tests

**File**: `tests/indexing/test_utxo_position_index.cpp`

```cpp
TEST(UTXOPositionIndex, AddAndRetrieve) {
    UTXOPositionIndex index;
    TxId txid = /* generate */;

    index.AddPosition(txid, 0, 12345);

    auto position = index.GetPosition(txid, 0);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(*position, 12345);
}

TEST(UTXOPositionIndex, RemoveReturnsPosition) {
    UTXOPositionIndex index;
    TxId txid = /* generate */;

    index.AddPosition(txid, 0, 99999);
    auto removed = index.RemovePosition(txid, 0);

    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 99999);

    // Should be gone
    EXPECT_FALSE(index.GetPosition(txid, 0).has_value());
}
```

### Integration Tests

**Invariant Test** (CRITICAL):
```cpp
TEST(PositionIndex, InvariantCheck) {
    // For every unspent UTXO in position index:
    auto* position_index = chainstate->GetUTXOPositionIndex();
    auto* forest = chainstate->utreexoForest();
    auto* chaindb = chainstate->GetChainDB();

    auto all_positions = position_index->GetAllPositions();

    for (const auto& [txid, vout, position] : all_positions) {
        // 1. UTXO must exist in ChainDB
        auto coin = chaindb->GetCoin(txid, vout);
        ASSERT_TRUE(coin.has_value()) << "Position exists but UTXO missing";

        // 2. Position must be valid in forest
        auto utxo_hash = ComputeUTXOHash(txid, vout, *coin);
        bool valid = forest->VerifyLeaf(position, utxo_hash);
        ASSERT_TRUE(valid) << "Position exists but forest verification failed";
    }
}
```

This catches:
- Ordering bugs
- Missed outputs
- Reorg corruption
- Silent consensus drift

---

## Summary

### What Exists ✅
- ✅ UTXOPositionIndex class (fully implemented)
- ✅ Clean API (Add, Remove, Get)
- ✅ Thread-safe implementation
- ✅ Undo data structures designed
- ✅ Build integration (compiles successfully)

### What's Needed (Next Step)
1. Wire into ChainstateService (add member + accessor)
2. Find the ConnectBlock location
3. Capture positions from `forest->add()` return value
4. Call `position_index->AddPosition()` for new UTXOs
5. Call `position_index->RemovePosition()` for spent UTXOs
6. Wire DisconnectBlock for reorg safety
7. Add invariant test

### What Becomes Trivial After Wiring
- ✅ GlobalUTXOSet::GenerateProof() - 3 lines of code
- ✅ Batch RPC proof generation - simple loop
- ✅ Stateless node proof serving - works automatically
- ✅ Explorer integration - query positions for any UTXO

---

**The foundation is built. The API is ready. Now we wire it.**
