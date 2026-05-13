# Phase 6B: BlockAcceptor Integration Strategy

## Problem Statement

BlockAcceptor and BlockValidator operate at different layers:

- **BlockAcceptor**: Consensus layer using ChainDB (RocksDB) for UTXO storage
- **BlockValidator**: Wallet layer using UTXOIndex (SQLite) for UTXO tracking

This layering mismatch prevents direct integration of Phase 6B parallel validation.

## Architecture Analysis

### Current Flow (BlockAcceptor::ConnectBlock)

```
1. Validate block header (PoW, timestamp, chainwork)
2. FOR EACH transaction:
   a. Parse transaction
   b. FOR EACH input:
      - chain_db->deleteCoin() [RocksDB write]
   c. FOR EACH output:
      - chain_db->putCoin() [RocksDB write]
3. Store block header
4. Update chain index
5. Commit RocksDB WriteBatch atomically
```

### Phase 6B Design (ParallelBlockValidator)

```
1. Parallel validation (read-only):
   - Script verification
   - UTXO existence checks
   - Signature validation
2. Serial application (write):
   - UTXOIndex->SpendUTXO()
   - UTXOIndex->AddUTXO()
```

### The Mismatch

- Phase 6B expects **UTXOIndex** (in-memory SQLite)
- BlockAcceptor uses **ChainDB** (RocksDB with WriteBatch)

## Integration Options

### Option A: Two-Phase Validation (Recommended)

Keep BlockAcceptor's RocksDB logic but add parallel validation phase.

**Implementation:**

```cpp
bool BlockAcceptor::ConnectBlock(const ParsedBlock& block, ...) {
    // ========== PHASE 1: Parallel Validation (Read-Only) ==========

    // Convert ParsedBlock → dinero::Block
    dinero::Block dinero_block = ConvertParsedBlockToBlock(block);

    // Parallel script verification and existence checks
    if (ctx_->parallel_validator) {
        std::string validation_error;
        if (!ctx_->parallel_validator->validateBlock(dinero_block, validation_error)) {
            error = "Validation failed: " + validation_error;
            return false;
        }
    }

    // ========== PHASE 2: Serial UTXO Updates (Existing Code) ==========

    // Create RocksDB WriteBatch
    rocksdb::WriteBatch batch;

    // Process each transaction (existing code)
    for (size_t tx_idx = 0; tx_idx < block.transactions.size(); tx_idx++) {
        // ... existing UTXO spend/create logic ...
        chain_db->deleteCoin(prevTxid, vout, &batch);
        chain_db->putCoin(txidU256, vout, coin, &batch);
    }

    // Commit atomically
    chain_db->WriteBatch(batch);

    return true;
}
```

**Benefits:**
- ✅ Minimal code changes
- ✅ Parallel script verification and signature checks
- ✅ Keeps existing RocksDB atomic commit logic
- ✅ No behavioral changes to UTXO management

**Limitations:**
- ⚠️ UTXO reads still happen serially during Phase 2
- ⚠️ Some duplication (validation checks UTXO existence, then Phase 2 reads again)

### Option B: Unified ChainDB-Based BlockValidator (Future Work)

Create a new `ConsensusBlockValidator` that works directly with ChainDB.

**Architecture:**

```cpp
namespace dinero {
namespace consensus {

class ConsensusBlockValidator {
public:
    explicit ConsensusBlockValidator(ChainDB* chain_db, ChainstateGuard* guard);

    // Parallel validation (read-only RocksDB queries)
    bool validateBlock(const Block& block, std::string& error);

    // Serial application (WriteBatch operations)
    bool connectBlock(const Block& block, rocksdb::WriteBatch& batch, std::string& error);

private:
    ChainDB* chain_db_;
    ChainstateGuard* chainstate_guard_;
};

} // namespace consensus
} // namespace dinero
```

**Benefits:**
- ✅ Clean separation of validation and application
- ✅ Works at consensus layer (ChainDB)
- ✅ Full parallel validation of all operations

**Limitations:**
- ⚠️ Requires significant refactoring (~800 lines)
- ⚠️ Must duplicate UTXO lookup logic from BlockAcceptor
- ⚠️ Risk of introducing bugs in consensus-critical code

### Option C: ValidationQueue for IBD Only

Use Phase 6B only during Initial Block Download, keep existing code for normal operation.

**Implementation:**

```cpp
// In daemon startup
if (isSyncing) {
    // Use ValidationQueue for IBD (full pipeline)
    ctx->validation_queue = std::make_shared<ValidationQueue>(...);
    ctx->validation_queue->start();
} else {
    // Use existing BlockAcceptor for normal operation
    // Optionally add parallel validation as in Option A
}
```

**Benefits:**
- ✅ Maximum IBD speedup (3-5×)
- ✅ No risk to normal operation
- ✅ Can use existing BlockValidator (UTXOIndex)

**Limitations:**
- ⚠️ Two code paths to maintain
- ⚠️ ValidationQueue needs UTXOIndex, but IBD uses ChainDB

## Recommended Approach

**Phase 6B.2: Two-Phase Validation (Option A)**

This is the safest and most practical approach:

1. ✅ Add parallel script verification to BlockAcceptor
2. ✅ Keep existing RocksDB UTXO management unchanged
3. ✅ Minimal risk, maximum benefit

### Implementation Steps

#### Step 1: Add ParallelBlockValidator initialization

```cpp
// src/daemon/main.cpp (or wherever DaemonContext is initialized)

#include "consensus/parallel_block_validator.h"

// After initializing chainstate
ctx->chainstate_guard = std::make_shared<dinero::consensus::ChainstateGuard>();

// Note: ParallelBlockValidator needs UTXOIndex, which is separate from ChainDB
// For now, skip ParallelBlockValidator and just use validation at transaction level
```

#### Step 2: Add parallel transaction validation to BlockAcceptor

Instead of using ParallelBlockValidator directly, we can parallelize at the transaction level within BlockAcceptor:

```cpp
#include "consensus/validation_worker_pool.h"
#include "consensus/transaction_validator.h"

bool BlockAcceptor::ConnectBlock(const ParsedBlock& block, ...) {
    // ... existing header validation ...

    // ========== Parallel Transaction Validation ==========

    if (block.transactions.size() > 10 && ctx_->parallel_validator) {
        // Large block: validate transactions in parallel

        std::vector<dinero::consensus::ValidationTask> tasks;
        for (size_t tx_idx = 1; tx_idx < block.transactions.size(); ++tx_idx) {
            dinero::Transaction tx;
            std::string parse_error;
            if (!TransactionParser::ParseTransaction(block.transactions[tx_idx], tx, parse_error)) {
                error = "Parse failed: " + parse_error;
                return false;
            }

            // Create validation task for this transaction
            dinero::consensus::ValidationTask task;
            task.type = dinero::consensus::ValidationTask::Type::CUSTOM;
            task.custom_func = [tx, height, chain_db](std::string& err) -> bool {
                // Validate transaction scripts and signatures
                auto result = dinero::consensus::TransactionValidator::ValidateTransaction(
                    tx, /* utxo_set */ nullptr, height
                );
                if (!result.valid) {
                    err = result.error;
                    return false;
                }
                return true;
            };

            tasks.push_back(std::move(task));
        }

        // Submit to worker pool and wait for results
        // (worker pool accessed via parallel_validator)
    }

    // ========== Serial UTXO Updates (Existing Code) ==========

    rocksdb::WriteBatch batch;

    // ... existing UTXO spend/create logic ...

    return true;
}
```

## Current Status

✅ **Completed:**
- ChainstateGuard added to DaemonContext
- ValidationQueue accepts ChainstateGuard parameter
- ParallelBlockValidator properly integrated with context
- Documentation created

⏳ **In Progress:**
- BlockAcceptor integration strategy documented

⏸️ **Deferred (Future Phase):**
- ConsensusBlockValidator (ChainDB-based)
- Full ValidationQueue integration with ChainDB
- End-to-end parallel block application

## Recommendation

For **Phase 6B.2**, implement **Option A: Two-Phase Validation**:

1. Add parallel script verification before UTXO updates
2. Keep existing RocksDB WriteBatch logic
3. Measure speedup (expect 1.5-2× for blocks with 20+ transactions)

For **Phase 6C** (future), consider:
- ConsensusBlockValidator that works directly with ChainDB
- Full parallel UTXO existence checks
- Target: 3-5× IBD speedup

This incremental approach minimizes risk while delivering immediate performance improvements.

---

**Status:** Architecture analysis complete, awaiting user decision on integration strategy.
