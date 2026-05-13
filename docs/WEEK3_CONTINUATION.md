# 🧩 Week 3 Continuation: State-Layer Context Migration

**Date**: 2025-11-06  
**Status**: 🚀 In Progress (GBTWorkManager Done ✅)

**Goal**: Remove all Chainstate / Mempool / Wallet globals → use DaemonContext injected services.

---

## ✅ Completed So Far

| Component | Status | Notes |
|-----------|--------|-------|
| GBTWorkManager | ✅ Done | Context injected, 4 globals replaced |
| ChainstateService / MempoolService / WalletService shells | ✅ Exist | Used by GBTWorkManager via `ctx->chainstate` etc. |
| ChainstateService accessors | ✅ Done | `chainDB()`, `utxoIndex()`, `blockchain()` methods available |

---

## 📋 Remaining Critical Migrations

### 1️⃣ peer_manager.cpp (5 globals)

**Files**: `src/daemon/p2p/peer_manager.cpp`

**Globals**: `g_chain_db_direct` (5 usages, lines 377-417)

**🔧 Before**
```cpp
if (dinero::g_chain_db_direct) {
    auto tip_result = dinero::g_chain_db_direct->getTip();
    // ...
    auto hash_result = dinero::g_chain_db_direct->getBlockHashByHeight(height);
}
```

**✅ After**
```cpp
if (ctx_ && ctx_->chainstate) {
    auto& chain_db = ctx_->chainstate->chainDB();
    auto tip_result = chain_db->getTip();
    // ...
    auto hash_result = chain_db->getBlockHashByHeight(height);
}
```

**🪜 Steps**
1. Add `DaemonContext* ctx_ = nullptr;` member to `PeerManager` class
2. Add `void SetContext(DaemonContext* ctx) { ctx_ = ctx; }` method
3. Replace all `dinero::g_chain_db_direct` calls with `ctx_->chainstate->chainDB()`
4. Wire context in `P2PService::Init()`:
   ```cpp
   // In P2PService::Init()
   peer_manager_->SetContext(&ctx);
   ```

**🧪 Test**
```bash
dinero-cli getpeerinfo
dinero-cli getblockcount
```
Expect normal peer handshake and block locator responses.

---

### 2️⃣ blockchain.cpp (7 globals)

**Files**: `src/daemon/blockchain.cpp`

**Globals**: `g_wallet_manager` (7 usages, lines 984-1052)

**🔧 Before**
```cpp
if (g_wallet_manager) {
    g_wallet_manager->NotifyBlockConnected(block);
    // ...
    if (g_wallet_manager->isScriptMine(scriptPubKeyHex)) {
        g_wallet_manager->addTransaction(...);
        g_wallet_manager->addUTXO(...);
    }
    g_wallet_manager->setBlockchainHeight(new_height);
}
```

**✅ After**
```cpp
if (ctx_ && ctx_->wallet) {
    auto& wallet = ctx_->wallet->get();
    wallet.NotifyBlockConnected(block);
    // ...
    if (wallet.isScriptMine(scriptPubKeyHex)) {
        wallet.addTransaction(...);
        wallet.addUTXO(...);
    }
    wallet.setBlockchainHeight(new_height);
}
```

**🪜 Steps**
1. Add `DaemonContext* ctx_` to `Blockchain` constructor
2. Update `Blockchain::SetContext(DaemonContext* ctx)` and store it
3. Replace all `g_wallet_manager` calls with `ctx_->wallet->get()` forms
4. Wire context in `ChainstateService::Init()`:
   ```cpp
   // In ChainstateService::Init()
   blockchain_->SetContext(&ctx);
   ```

**🧪 Test**
```bash
dinero-cli getwalletinfo
dinero-cli getbalance
# Mine a block
dinero-cli mining.generatetoaddress 1 "din1q..."
# Check balance updated
dinero-cli getbalance
```
Balances should update after mining a block.

---

### 3️⃣ mining_safety_gates.cpp (10 globals)

**Files**: `src/daemon/mining_safety_gates.cpp`

**Globals**: `g_wallet_manager`, `g_chain_db_direct`

**🔧 Before**
```cpp
extern dinero::ChainDB* g_chain_db_direct;
if (!g_chain_db_direct->IsTipSynced()) return false;

extern std::unique_ptr<dinero::WalletManager> g_wallet_manager;
if (g_wallet_manager->IsLocked()) return false;
```

**✅ After**
```cpp
if (!ctx_ || !ctx_->chainstate || !ctx_->wallet) return false;

auto& chain_db = ctx_->chainstate->chainDB();
if (!chain_db->IsTipSynced()) return false;

auto& wallet = ctx_->wallet->get();
if (wallet.IsLocked()) return false;
```

**🪜 Steps**
1. Add `DaemonContext* ctx_` member + setter to `MiningSafetyGates` class
2. Replace all globals with context access:
   - `g_chain_db_direct` → `ctx_->chainstate->chainDB()`
   - `g_wallet_manager` → `ctx_->wallet->get()`
3. Wire context in `MiningCoordinatorService::Init()`:
   ```cpp
   // In MiningCoordinatorService::Init()
   safety_gates_->SetContext(&ctx);
   ```

**🧪 Test**
```bash
dinero-cli getmininginfo
dinero-cli getblocktemplate
# Lock wallet
dinero-cli wallet.lock
# Mining should stop
dinero-cli getmininginfo
```
Mining should start only when wallet is unlocked and chain is synced.

---

### 4️⃣ block_acceptor.cpp (42 globals ⚠️ Heavy)

**Files**: `src/daemon/block_acceptor.cpp`

**Globals**: `g_chain_db_direct`, `g_utxo_set_direct`, `g_wallet_manager`

**🔧 Before**
```cpp
extern dinero::ChainDB* g_chain_db_direct;
if (!g_chain_db_direct->CheckBlockHeader(block))
    return false;

extern UTXOIndex* g_utxo_set_direct;
ApplyUTXOChanges(g_utxo_set_direct, block);

extern std::unique_ptr<dinero::WalletManager> g_wallet_manager;
g_wallet_manager->NotifyBlockAccepted(block);
```

**✅ After**
```cpp
if (!ctx_ || !ctx_->chainstate || !ctx_->wallet) return false;

auto& chain_db = ctx_->chainstate->chainDB();
auto& utxo = ctx_->chainstate->utxoIndex();
auto& wallet = ctx_->wallet->get();

if (!chain_db->CheckBlockHeader(block))
    return false;

ApplyUTXOChanges(utxo, block);
wallet.NotifyBlockAccepted(block);
```

**🪜 Steps**
1. Add `DaemonContext* ctx_` member to `BlockAcceptor` class
2. Add `void SetContext(DaemonContext* ctx)` method
3. Replace each global with its context counterpart:
   - `g_chain_db_direct` → `ctx_->chainstate->chainDB()`
   - `g_utxo_set_direct` → `ctx_->chainstate->utxoIndex()`
   - `g_wallet_manager` → `ctx_->wallet->get()`
4. Wire context in `ChainstateService::Init()` or `P2PService::Init()`:
   ```cpp
   // Wherever BlockAcceptor is created
   block_acceptor_->SetContext(&ctx);
   ```
5. Validate interactions with WalletService and MempoolService

**🧪 Test**
```bash
dinero-cli getblockchaininfo
dinero-cli getrawmempool
# Submit a block
dinero-cli submitblock "..."
# Check UTXO updates
dinero-cli gettxoutsetinfo
```
Block acceptance and UTXO updates should behave identically to pre-migration.

---

## 🧱 Testing Phases

| Phase | Command | Expected Result |
|-------|---------|-----------------|
| 1 | `make / cmake --build .` | ✅ Build success (no link errors) |
| 2 | `dinero-cli getblockcount` | Matches previous height |
| 3 | `dinero-cli mining.generatetoaddress 1 "din1q..."` | Block mines successfully |
| 4 | `dinero-cli getbalance` | Wallet reflects reward |
| 5 | `Ctrl-C / graceful exit` | No crash / clean shutdown |

---

## 📊 Progress Tracker

| File | Lines Migrated | Status |
|------|----------------|--------|
| `peer_manager.cpp` | 5 | ⬜ Pending |
| `blockchain.cpp` | 7 | ⬜ Pending |
| `mining_safety_gates.cpp` | 10 | ⬜ Pending |
| `block_acceptor.cpp` | 42 | ⬜ Pending |
| **Total Remaining** | **64 lines** | **In Progress** |

---

## 🔚 End of Week 3 Target

- ✅ All stateful modules migrated to DaemonContext
- ✅ No more direct usage of `g_chain_db_direct`, `g_wallet_manager`, or `g_utxo_set_direct`
- ✅ Daemon runs with clean startup/shutdown
- ✅ RPC commands (`getblockcount`, `getbalance`, `getmininginfo`) still functional
- ✅ Ready for Week 4 → P2P + RPC Network Layer migration

---

## 🎯 Migration Order Recommendation

1. **Start with `peer_manager.cpp`** (5 globals, isolated, low risk)
2. **Then `mining_safety_gates.cpp`** (10 globals, validation logic)
3. **Then `blockchain.cpp`** (7 globals, wallet integration)
4. **Finally `block_acceptor.cpp`** (42 globals, most complex)

**Why this order?**
- `peer_manager.cpp` is isolated P2P code, easy to test
- `mining_safety_gates.cpp` is validation-only, no side effects
- `blockchain.cpp` has wallet hooks but is well-contained
- `block_acceptor.cpp` touches everything, do it last when others are stable

---

## 💡 Tips for Migration

### 1. **One File at a Time**
Don't try to migrate everything at once. Pick one file, migrate it, test it, commit it.

### 2. **Keep Bridge While Migrating**
Don't remove `legacy_globals_stub.cpp` until EVERYTHING is migrated. The bridge lets old and new code coexist.

### 3. **Use Compiler Errors as Guide**
Once you remove a global, the compiler will tell you every place that used it. Fix them one by one.

### 4. **Write Migration Tests**
Before migrating a component, write a test that uses it. After migration, the test should still pass.

### 5. **Document As You Go**
Add comments explaining why you changed from globals to context:
```cpp
// MIGRATED Week 3: Now uses ctx_->chainstate instead of g_chain_db_direct
auto& chain_db = ctx_->chainstate->chainDB();
```

---

## 🔍 Finding Remaining Globals

After each migration, verify no globals remain:

```bash
# Find remaining global usages
grep -r "g_chain_db_direct" src/ --include="*.cpp" | grep -v "legacy_globals"
grep -r "g_wallet_manager" src/ --include="*.cpp" | grep -v "legacy_globals"
grep -r "g_utxo_set_direct" src/ --include="*.cpp" | grep -v "legacy_globals"

# Count remaining usages
grep -r "g_chain_db_direct\|g_wallet_manager\|g_utxo_set_direct" src/ --include="*.cpp" | wc -l
```

**Target**: 0 usages (excluding `legacy_globals_stub.cpp`)

---

## 📝 Next Action

👉 `git add docs/WEEK3_CONTINUATION.md`  
👉 `git commit -m "Week 3 continuation plan: state-layer context migration checklist added"`

**Ready to start migration?** Begin with `peer_manager.cpp` - it's the safest first step! 🚀

