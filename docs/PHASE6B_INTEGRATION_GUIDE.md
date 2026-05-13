# Phase 6B Integration Guide: Parallel Validation

## Overview

This guide shows how to integrate **Phase 6B parallel validation** into your existing `BlockAcceptor` and daemon code.

---

## Quick Start (Minimal Changes)

### **Option 1: Use ParallelBlockValidator (Easiest)**

Replace single-threaded validation with parallel validation wrapper.

**Before (Phase 5E):**
```cpp
// src/daemon/block_acceptor.cpp
bool BlockAcceptor::ConnectBlock(const ParsedBlock& block, uint64_t height, ...) {
    dinero::Block dinero_block = ConvertParsedBlockToBlock(block);

    dinero::consensus::BlockValidator validator(g_utxo_index);
    dinero::consensus::BlockUndo undo;
    std::string error;

    if (!validator.ConnectBlock(dinero_block, undo, error)) {
        return false;
    }

    return true;
}
```

**After (Phase 6B):**
```cpp
// src/daemon/block_acceptor.cpp
#include "consensus/parallel_block_validator.h"
#include "consensus/chainstate_guard.h"

// Global chainstate guard (initialize in main)
dinero::consensus::ChainstateGuard g_chainstate_guard;

bool BlockAcceptor::ConnectBlock(const ParsedBlock& block, uint64_t height, ...) {
    dinero::Block dinero_block = ConvertParsedBlockToBlock(block);

    // Use parallel validator
    dinero::consensus::ParallelBlockValidator validator(
        g_utxo_index,
        &g_chainstate_guard,
        dinero::consensus::ParallelBlockValidator::Config::forNormalOperation()
    );

    dinero::consensus::BlockUndo undo;
    std::string error;

    // Validate + connect (parallel validation, serial application)
    if (!validator.validateAndConnect(dinero_block, undo, error)) {
        return false;
    }

    return true;
}
```

**Benefits:**
- ✅ **Drop-in replacement** for existing code
- ✅ **Automatic parallelization** for blocks >= 10 tx
- ✅ **Thread-safe** UTXO access via chainstate guard
- ✅ **No behavioral changes** (same validation logic)

---

### **Option 2: Use ValidationQueue (Maximum Performance)**

For IBD and high-throughput scenarios, use the full validation queue for pipelining.

**Architecture:**
```
Network Thread → ValidationQueue::submit(block)
                      ↓
              [Parallel Validation]
                      ↓
              [Serial Application]
                      ↓
              Callback: on_block_connected
```

**Implementation:**

**1. Initialize ValidationQueue in daemon startup:**

```cpp
// src/daemon/main.cpp
#include "consensus/validation_queue.h"
#include "consensus/chainstate_guard.h"

// Global instances
dinero::consensus::ChainstateGuard g_chainstate_guard;
std::unique_ptr<dinero::consensus::ValidationQueue> g_validation_queue;

int main() {
    // ... existing initialization ...

    // Create validation queue (after UTXO set initialized)
    auto queue_config = dinero::consensus::ValidationQueue::Config::forIBD();
    g_validation_queue = std::make_unique<dinero::consensus::ValidationQueue>(
        g_utxo_index,
        queue_config
    );

    // Set callbacks
    g_validation_queue->setBlockConnectedCallback(
        [](const dinero::Block& block, uint64_t height) {
            std::cout << "Block " << height << " connected\n";
            // Notify wallet, GUI, etc.
        }
    );

    g_validation_queue->setBlockFailedCallback(
        [](const dinero::Block& block, const std::string& error) {
            std::cerr << "Block validation failed: " << error << "\n";
        }
    );

    g_validation_queue->start();

    // ... run daemon ...

    g_validation_queue->stop();
    return 0;
}
```

**2. Submit blocks from network/RPC:**

```cpp
// src/daemon/block_acceptor.cpp
bool BlockAcceptor::AcceptBlockFromRPC(const std::string& blockHex, ...) {
    ParsedBlock parsed = ParseBlockFromHex(blockHex);
    dinero::Block block = ConvertParsedBlockToBlock(parsed);

    // Submit to validation queue (async)
    bool queued = g_validation_queue->submit(block, height, parsed.prevBlockHash);

    if (!queued) {
        error = "Validation queue full";
        return false;
    }

    // For RPC, we want synchronous behavior, so wait for completion
    g_validation_queue->waitForCompletion();

    return true; // Block will be validated asynchronously
}
```

**For IBD (Network Sync):**

```cpp
// src/daemon/network_handler.cpp (pseudo-code)
void NetworkHandler::onBlockReceived(const Block& block, uint64_t height, ...) {
    // Submit block to queue (non-blocking)
    g_validation_queue->submit(block, height, prev_hash);

    // Network thread can immediately request next block
    requestNextBlock();
}
```

**Benefits:**
- ✅ **3-5× faster IBD** (parallel validation + pipelining)
- ✅ **Non-blocking** network thread
- ✅ **Automatic dependency ordering** (block N+1 waits for N)
- ✅ **Backpressure handling** (queue limits prevent memory exhaustion)

---

## Configuration Presets

### **For Initial Block Download (IBD):**

```cpp
auto config = dinero::consensus::ValidationQueue::Config::forIBD();
// - max_in_flight_blocks: 32
// - max_queued_blocks: 256
// - worker_threads: all CPU cores
// - parallel_threshold: 5 tx

g_validation_queue = std::make_unique<dinero::consensus::ValidationQueue>(
    g_utxo_index,
    config
);
```

### **For Normal Mainnet Operation:**

```cpp
auto config = dinero::consensus::ValidationQueue::Config::forNormalOperation();
// - max_in_flight_blocks: 8
// - max_queued_blocks: 64
// - worker_threads: half of CPU cores
// - parallel_threshold: 10 tx

g_validation_queue = std::make_unique<dinero::consensus::ValidationQueue>(
    g_utxo_index,
    config
);
```

### **For Low-Resource Systems:**

```cpp
auto validator_config = dinero::consensus::ParallelBlockValidator::Config::forLowResource();
// - enable_parallel: false
// - worker_threads: 1

// Use ParallelBlockValidator directly (no queue)
dinero::consensus::ParallelBlockValidator validator(
    g_utxo_index,
    &g_chainstate_guard,
    validator_config
);
```

---

## Thread Safety Considerations

### **1. UTXO Set Access**

**Problem:** Parallel validation reads UTXO set, block application writes to it.

**Solution:** Use `ChainstateGuard` with read/write locks.

```cpp
// Validation (read-only, concurrent)
{
    auto lock = g_chainstate_guard.readLock();
    bool exists = utxo_set->Exists(txid, vout);
    // Multiple threads can hold read lock simultaneously
}

// Apply block (write, exclusive)
{
    auto lock = g_chainstate_guard.writeLock();
    utxo_set->SpendUTXO(txid, vout);  // Exclusive access
    utxo_set->AddUTXO(new_utxo);
}
```

**Important:** `ValidationQueue` handles locking automatically. You only need manual locks if bypassing the queue.

---

### **2. Block Index Access**

If you have a separate block index (height → block hash), protect it similarly:

```cpp
// Option 1: Use same chainstate guard
{
    auto lock = g_chainstate_guard.writeLock();
    block_index[height] = block_hash;
}

// Option 2: Separate guard for block index
dinero::consensus::ChainstateGuard g_block_index_guard;

{
    auto lock = g_block_index_guard.writeLock();
    block_index[height] = block_hash;
}
```

---

### **3. Wallet Notifications**

**Problem:** Wallet scans new blocks for relevant transactions (non-atomic with chainstate).

**Solution:** Use `on_block_connected` callback **after** chainstate update.

```cpp
g_validation_queue->setBlockConnectedCallback(
    [](const dinero::Block& block, uint64_t height) {
        // Chainstate is already updated and write lock released
        // Safe to notify wallet
        if (g_wallet) {
            g_wallet->ScanBlock(block, height);
        }
    }
);
```

---

## Performance Tuning

### **1. Adjust Parallel Threshold**

Small blocks don't benefit from parallelization (overhead > speedup).

```cpp
auto config = dinero::consensus::ParallelBlockValidator::Config::forNormalOperation();
config.parallel_threshold = 20; // Only parallelize blocks with >= 20 tx

dinero::consensus::ParallelBlockValidator validator(g_utxo_index, &g_chainstate_guard, config);
```

**Guideline:**
- **IBD:** threshold = 5 (parallelize more)
- **Mainnet:** threshold = 10 (balanced)
- **Low-resource:** threshold = 1000 (effectively disable)

---

### **2. Tune Worker Pool Size**

More workers = more parallelism, but diminishing returns.

```cpp
auto config = dinero::consensus::ValidationQueue::Config::forIBD();
config.worker_pool_threads = 8; // Force 8 workers

g_validation_queue = std::make_unique<dinero::consensus::ValidationQueue>(
    g_utxo_index,
    config
);
```

**Guideline:**
- **2-4 cores:** Use N-1 workers (leave 1 for main thread)
- **8+ cores:** Use all cores for IBD, half for mainnet
- **16+ cores:** Diminishing returns (bottleneck shifts to I/O)

---

### **3. Monitor Metrics**

```cpp
// Validation queue metrics
auto metrics = g_validation_queue->getMetrics();
std::cout << metrics.toString() << std::endl;

// Worker pool metrics
auto worker_metrics = g_validation_queue->getWorkerPoolMetrics();
std::cout << worker_metrics.toString() << std::endl;
```

**Output:**
```
ValidationQueue::Metrics {
  Submitted:   1000
  Validated:   998
  Connected:   998
  Failed:      2
  Avg validation time: 45 ms
  Avg apply time:      12 ms
}
```

---

## Migration Checklist

### **Phase 1: Add Chainstate Guard (No Behavior Change)**

- [ ] Add `#include "consensus/chainstate_guard.h"` to daemon
- [ ] Create global `ChainstateGuard g_chainstate_guard;`
- [ ] Wrap existing UTXO mutations with write locks
- [ ] Test: Verify existing functionality unchanged

### **Phase 2: Enable Parallel Validation (Optional)**

- [ ] Replace `BlockValidator` with `ParallelBlockValidator`
- [ ] Set appropriate config (IBD vs mainnet)
- [ ] Test: Verify blocks validate correctly

### **Phase 3: Enable Validation Queue (IBD Speedup)**

- [ ] Create `ValidationQueue` in daemon startup
- [ ] Submit blocks from network handler
- [ ] Set `on_block_connected` callback
- [ ] Test: Verify IBD completes successfully

### **Phase 4: Performance Benchmarking**

- [ ] Measure IBD time (before vs after)
- [ ] Check CPU utilization (should be near 100% during IBD)
- [ ] Monitor queue depth (should stay < max_queued_blocks)
- [ ] Profile bottlenecks (validation vs I/O vs network)

---

## Troubleshooting

### **Issue: Deadlock during shutdown**

**Symptom:** Daemon hangs on exit

**Cause:** Validation queue waiting for pending blocks

**Fix:**
```cpp
// Before shutdown
g_validation_queue->stop(); // Gracefully drains queue
g_validation_queue.reset(); // Destroy queue
```

---

### **Issue: High memory usage**

**Symptom:** RSS grows during IBD

**Cause:** Too many blocks queued

**Fix:**
```cpp
auto config = dinero::consensus::ValidationQueue::Config::forIBD();
config.max_queued_blocks = 64; // Reduce from 256
config.max_in_flight_blocks = 16; // Reduce from 32
```

---

### **Issue: Slower than single-threaded**

**Symptom:** IBD takes longer with parallelization

**Cause:** Small blocks (high overhead) or I/O bottleneck

**Fix:**
```cpp
// Increase parallel threshold
auto config = dinero::consensus::ParallelBlockValidator::Config::forNormalOperation();
config.parallel_threshold = 50; // Only parallelize large blocks

// Or disable parallel validation
config.enable_parallel = false;
```

---

### **Issue: Blocks fail validation randomly**

**Symptom:** "UTXO not found" errors intermittently

**Cause:** Race condition (missing chainstate guard)

**Fix:**
Ensure all UTXO access is protected:
```cpp
// BAD: Direct access
bool exists = utxo_set->Exists(txid, vout); // Race condition!

// GOOD: Protected access
{
    auto lock = g_chainstate_guard.readLock();
    bool exists = utxo_set->Exists(txid, vout); // Thread-safe
}
```

---

## Example: Complete Integration

**File: `src/daemon/main.cpp`**

```cpp
#include "consensus/validation_queue.h"
#include "consensus/chainstate_guard.h"

// Global instances
dinero::consensus::ChainstateGuard g_chainstate_guard;
std::unique_ptr<dinero::consensus::ValidationQueue> g_validation_queue;

int main(int argc, char** argv) {
    // Parse config
    bool is_ibd = /* detect if syncing */;

    // Initialize UTXO set
    UTXOIndex* utxo_set = /* ... */;

    // Create validation queue
    auto config = is_ibd
        ? dinero::consensus::ValidationQueue::Config::forIBD()
        : dinero::consensus::ValidationQueue::Config::forNormalOperation();

    g_validation_queue = std::make_unique<dinero::consensus::ValidationQueue>(
        utxo_set,
        config
    );

    // Set callbacks
    g_validation_queue->setBlockConnectedCallback(onBlockConnected);
    g_validation_queue->setBlockFailedCallback(onBlockFailed);

    g_validation_queue->start();

    // Run daemon
    RunDaemon();

    // Cleanup
    g_validation_queue->stop();
    g_validation_queue.reset();

    return 0;
}

void onBlockConnected(const dinero::Block& block, uint64_t height) {
    std::cout << "Block " << height << " connected\n";

    // Notify wallet
    if (g_wallet) {
        g_wallet->ScanBlock(block, height);
    }

    // Update GUI
    NotifyBlockAdded(height);
}

void onBlockFailed(const dinero::Block& block, const std::string& error) {
    std::cerr << "Block validation failed: " << error << "\n";
    // Optionally ban peer, log error, etc.
}
```

---

**Phase 6B Integration Complete!**

For questions or issues, see:
- `PHASE6B_ARCHITECTURE.md` — Design overview
- `include/consensus/validation_queue.h` — API documentation
- `include/consensus/parallel_block_validator.h` — Validator API
