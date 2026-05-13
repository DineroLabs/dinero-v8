# Phase D.3 - RPC ↔ Wallet Consistency Analysis

## Goal
Ensure RPC-visible state (`mining.start`, `mining.info`, `getbalance`) matches wallet reality.

---

## Issue #1: Mining Address Persistence ❌ CRITICAL

### Problem
`mining.start` RPC stores address in-memory only, not persisted to disk.

### Current Behavior
1. User calls `mining.start(threads, "din1...")`
2. Address stored in `MiningService` → `MiningManager` (memory only)
3. Daemon restarts
4. **Mining address lost** - `mining.info` returns empty address

### Root Cause
**File**: `src/rpc/methods_mining_context.cpp:214`
```cpp
// Reads from WalletManager (persistent SQLite)
if (address.empty()) {
    address = wallet->get().getMiningAddress(...);  // ✅ Reads from SQLite
}

// Writes to MiningService (volatile memory)
mining->setMiningAddress(address);  // ❌ Memory only!
```

**File**: `src/mining/mining_manager_v2.cpp:292-308`
```cpp
void MiningManager::setMiningAddress(const std::string& address) {
    std::lock_guard lock(address_mutex_);
    mining_address_ = address;  // ❌ Only in-memory storage
    // No persistence!
}
```

### Impact
- **UX Failure**: Mining address doesn't survive restarts
- **Asymmetric API**: Reads from persistent storage, writes to volatile storage
- **Trust Issue**: Users lose their configured mining address

### Fix Required
When `mining.start` sets an address, **also** persist it to `WalletManager`:

```cpp
// In rpc_context_mining_start(), after line 214:
mining->setMiningAddress(address);

// ADD: Persist to WalletManager so it survives restarts
if (ctx.daemon->wallet) {
    auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (wallet && wallet->hasActiveWallet()) {
        wallet->get().setMiningAddress(
            address,
            wallet->getCurrentWalletName(),
            dinero::ChainToString(dinero::ChainType::MAINNET)
        );
    }
}
```

---

## Issue #2: Balance Consistency ✅ VERIFIED

### Architecture Understanding
Two UTXO tracking systems exist for **different purposes**:

1. **WalletManager**: SQLite `utxos` table (wallet-specific UTXOs)
   - Purpose: Track UTXOs for addresses owned by the wallet
   - Used by: `getbalance` RPC

2. **UTXOIndex**: RocksDB UTXO set (global UTXO set)
   - Purpose: Consensus validation, blockchain state
   - Used by: Block validation, Phase D.2 integration tests

### Data Flow (Verified)
```
BlockAcceptor::acceptBlock()
  └→ WalletNotify::OnBlockConnected()  [block_acceptor.cpp:1928]
      └→ WalletWorker::QueueBlockConnected()  [wallet_worker.cpp:436]
          └→ WalletWorker::ProcessBlockConnected()
              └→ wallet_manager_->addUTXO()  [wallet_worker.cpp:264]
                  └→ INSERT INTO utxos (SQLite)
```

### Synchronization Status
- ✅ WalletManager receives block-connected events from BlockAcceptor
- ✅ WalletManager scans transactions for owned addresses
- ✅ WalletManager updates its `utxos` table independently
- ✅ Balance queries use correct coinbase maturity rules (100 blocks)

### Key Insight
These systems are **intentionally separate**:
- **UTXOIndex**: Global UTXO set (all addresses)
- **WalletManager**: Wallet UTXO set (only owned addresses)

Both track the same blockchain, but WalletManager filters for owned addresses only.

### RPC Correctness
`getbalance` RPC correctly returns WalletManager's view, which is:
- Synchronized with blockchain via WalletNotify
- Filtered to wallet-owned addresses only
- Properly accounts for coinbase maturity

**No issues found** - balance reporting is accurate.

---

## Issue #3: mining.info Accuracy ✅ VERIFIED

### Status
`mining.info` correctly returns:
- Real-time mining stats from `MiningManager::getStats()`
- Current mining address from `MiningService::getMiningAddress()`
- Blockchain height from `ChainstateService`
- Difficulty from `ChainDB`

**No issues found** - stats are accurate and real-time.

---

## Summary

| RPC Method | Issue | Severity | Status |
|------------|-------|----------|--------|
| `mining.start` | Address not persisted | **CRITICAL** | ✅ **FIXED** |
| `mining.info` | None | - | ✅ Verified |
| `getbalance` | Balance consistency | Low | ✅ Verified (intentional design) |

**Phase D.3 Complete**:
1. ✅ Fixed mining address persistence (Issue #1)
2. ✅ Verified balance consistency (Issue #2 - no fix needed)
3. ✅ Verified mining.info accuracy (Issue #3)

**Changes Made**:
- `src/rpc/methods_mining_context.cpp:216-231` - Added WalletManager persistence to `mining.start`
- `docs/PHASE_D3_RPC_WALLET_INCONSISTENCIES.md` - Documented findings and architecture

---

## Test Plan

### Test 1: Mining Address Persistence
```cpp
// Start mining with address
rpc_call("mining.start", [4, "din1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw"]);

// Restart daemon
restart_daemon();

// Verify address persisted
auto info = rpc_call("mining.info", {});
EXPECT_EQ("din1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw", info["address"]);
```

### Test 2: Balance Consistency
```cpp
// Mine block to wallet address
mine_block_to_address("din1...");

// Verify both systems agree
auto rpc_balance = rpc_call("getbalance", {});
auto utxo_balance = utxo_index->GetBalanceWithMaturity(height);

EXPECT_EQ(rpc_balance["immature"], utxo_balance.immature);
EXPECT_EQ(rpc_balance["confirmed"], utxo_balance.confirmed);
```
