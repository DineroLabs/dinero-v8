# Phase 10d: Utreexo Template Builder Integration

**Date:** 2026-01-16
**Status:** ✅ **COMPLETE**

---

## Executive Summary

Implemented optional Utreexo root computation in the block template builder. This allows blocks to optionally commit to UTXO state without changing miner logic.

---

## The Three-Layer Architecture (As Implemented)

```
┌─────────────────────────────────────────────────────────────┐
│ 1️⃣ TEMPLATE BUILDER (Has Utreexo Forest - LOCAL ONLY)      │
│                                                              │
│   UTXOs → Leaves → Trees → Forest → Accumulator → Root      │
│                                                    ▲         │
│                                           ONLY THIS SHARED   │
│                                                              │
│   • Clones forest                                            │
│   • Applies block transactions                               │
│   • Computes root: forest.getCommitment()                    │
│   • Puts root in header: header.utreexo_root = root          │
│   • Sends 128-byte header to miner                           │
└─────────────────────────────────────────────────────────────┘
                             ↓
                    128-byte header
                    (bytes 68-99 = root)
                             ↓
┌─────────────────────────────────────────────────────────────┐
│ 2️⃣ MINER (NO Utreexo Knowledge)                             │
│                                                              │
│   while (SHA256d(header) > target) {                         │
│       nonce++;                                               │
│   }                                                          │
│                                                              │
│   • Hashes ALL 128 bytes (including root)                    │
│   • Doesn't know what bytes 68-99 mean                       │
│   • Zero Utreexo logic                                       │
└─────────────────────────────────────────────────────────────┘
                             ↓
                  Mined block with nonce
                             ↓
┌─────────────────────────────────────────────────────────────┐
│ 3️⃣ VALIDATOR (Has Own Utreexo Forest - LOCAL ONLY)          │
│                                                              │
│   • Applies block to own forest                              │
│   • Computes expected_root = my_forest.getCommitment()       │
│   • Checks: if (root != 0 && root != expected) REJECT        │
│   • No shared forests/proofs between nodes                   │
└─────────────────────────────────────────────────────────────┘
```

---

## What Gets Shared vs What Stays Local

| Component | Goes On-Chain? | Purpose |
|-----------|----------------|---------|
| **Utreexo Root** | ✅ Yes (header bytes 68-99) | Commitment to UTXO state |
| **Utreexo Forest** | ❌ No (local only) | Data structure to compute root |
| **Leaves** | ❌ No (local only) | UTXO hashes in forest |
| **Trees** | ❌ No (local only) | Binary tree structure |
| **Accumulator** | ❌ No (local only) | Combined forest representation |

---

## Implementation Changes

### 1. BlockTemplateBuilder - Added Optional Forest

**File:** `include/mining/block_template.h`

```cpp
class BlockTemplateBuilder {
public:
    BlockTemplateBuilder(
        mempool::Mempool& mempool,
        consensus::CoinsDB& coins_db,
        const BlockTemplateConfig& config = BlockTemplateConfig(),
        consensus::UtreexoForest* utreexo_forest = nullptr  // ← NEW: Optional
    );

private:
    consensus::UtreexoForest* utreexo_forest_;  // ← NEW: Nullable pointer
};
```

### 2. Root Computation in createBlockTemplate()

**File:** `src/mining/block_template.cpp`

```cpp
if (utreexo_forest_) {
    // Clone the current forest to simulate applying this block
    auto forest_copy = utreexo_forest_->clone();

    // Apply block mutations:
    for (auto& tx : template_block->transactions) {
        // Remove spent UTXOs (tx inputs)
        for (auto& input : tx.vin) {
            auto coin = coins_db_.getCoin(input.prevout);
            auto leaf_hash = consensus::HashUTXO(...);
            auto proof = forest_copy.prove(position);
            forest_copy.remove(leaf_hash, proof);
        }

        // Add created UTXOs (tx outputs)
        for (auto& output : tx.vout) {
            auto leaf_hash = consensus::HashUTXO(...);
            forest_copy.add(leaf_hash);
        }
    }

    // Get the commitment (root) - THIS IS WHAT GOES IN HEADER
    auto root = forest_copy.getCommitment();  // 32 bytes
    template_block->block.header.utreexo_root = uint256(root);
} else {
    // Legacy mode - zero commitment
    template_block->block.header.utreexo_root = uint256();
}
```

### 3. RPC Integration - Pass Forest from Chainstate

**File:** `src/rpc/methods_mining_template.cpp`

```cpp
// Get Utreexo forest from chainstate (optional)
auto* utreexo_forest = chainstate_service->utreexoForest();

// Pass forest to template builder
dinero::mining::BlockTemplateBuilder builder(
    temp_mempool,
    temp_coins_db,
    dinero::mining::BlockTemplateConfig(),
    utreexo_forest  // ← NEW: Optional forest
);
```

**File:** `src/rpc/methods_miner_control.cpp` - Same change

---

## What Changed vs What Didn't

### ✅ Changed (Template Builder Only)

- BlockTemplateBuilder accepts optional UtreexoForest pointer
- If forest available, computes root and puts it in header
- If forest unavailable, uses zero root (legacy mode)
- RPC passes forest from ChainstateService to builder

### ❌ Unchanged (Miners, Consensus, Everything Else)

- **Miners:** Still hash 128 bytes blindly (no Utreexo knowledge)
- **Stratum:** Already fixed for 128-byte headers (Phase 10a)
- **GPU miners:** Disabled (Phase 10b - kernels broken for 128 bytes)
- **CPU miner:** No changes needed (already hashes full header)
- **Consensus validation:** Already enforces root matching (Phase 9)
- **Block header format:** Already has utreexo_root field (Phase 3)

---

## Sanity Checks

### ✅ "If I delete all Utreexo code, does mining still work?"

**YES.** Just set `utreexo_forest = nullptr` → zero root → legacy mining.

### ✅ "If miner was a dumb ASIC seeing only bytes, would this work?"

**YES.** Miner receives 128 bytes, hashes them, doesn't interpret them.

### ✅ "Does the miner compute/validate/store the accumulator?"

**NO.** Miner only hashes bytes. Template builder computes root from its local forest.

---

## Testing

### Compilation Status

✅ **dinero-miner:** Compiles successfully
✅ **dinero_rpc_handlers:** Compiles successfully
✅ **dinero_consensus:** Compiles successfully
✅ **dinero_chainstate:** Compiles successfully

### What to Test Next

1. **Regtest mining:** Mine blocks with Utreexo enabled, verify root is non-zero
2. **Legacy mining:** Mine blocks without forest, verify root is zero
3. **Validation:** Ensure validators with forest accept blocks with correct root
4. **Rejection:** Ensure validators reject blocks with incorrect root

---

## Files Modified

1. `include/mining/block_template.h` - Added forest parameter
2. `src/mining/block_template.cpp` - Implemented root computation
3. `src/rpc/methods_mining_template.cpp` - Pass forest from chainstate
4. `src/rpc/methods_miner_control.cpp` - Pass forest from chainstate

---

## One-Sentence Summary

**Utreexo's accumulator is how the template builder arrives at the root; the root is the only thing the miner ever sees.**

---

## Architecture Compliance

✅ **Separation of Concerns**
- Template builder: Computes root
- Miner: Hashes bytes
- Validator: Verifies root

✅ **No Miner Changes**
- Miner still hashes 128 bytes
- No Utreexo knowledge required
- Works with zero or non-zero root

✅ **Optional Integration**
- Forest pointer is nullable
- Zero root = legacy mode
- Non-zero root = Utreexo mode

✅ **Local-Only Accumulators**
- Each node has own forest
- Forest never transmitted
- Only root goes on-chain

---

**Implementation Status:** ✅ COMPLETE
**Next Phase:** Test in regtest environment

🚨 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>
