# RPC Handler File Analysis & Cleanup Plan
## Date: November 7, 2025

---

## 📋 File Categories

### Category 1: **KEEP** - Context-Aware Handlers (Modern Implementation)

These files correctly use `ctx.daemon->service` pattern and should be **retained**.

| File | Handlers | Pattern | Status |
|------|----------|---------|--------|
| `methods_blockchain_context.cpp` | 10 blockchain methods | ✅ Context-aware | **KEEP** |
| `methods_wallet_context.cpp` | 39 wallet methods | ✅ Context-aware | **KEEP** |
| `methods_mining_context.cpp` | 8 mining methods | ✅ Context-aware | **KEEP** |
| `methods_mempool_context.cpp` | 6 mempool methods | ✅ Context-aware | **KEEP** |
| `methods_network_context.cpp` | 7 network methods | ✅ Context-aware | **KEEP** |
| `methods_economics_context.cpp` | 6 economics methods | ✅ Context-aware | **KEEP** |
| `methods_consensus_context.cpp` | Consensus methods | ✅ Context-aware | **KEEP** |
| `methods_payment_context.cpp` | Payment methods | ✅ Context-aware | **KEEP** |
| `methods_sync_context.cpp` | Sync methods | ✅ Context-aware | **KEEP** |
| `methods_market_context.cpp` | Market methods | ✅ Context-aware | **KEEP** |
| `methods_bridge_context.cpp` | Bridge methods | ✅ Context-aware | **KEEP** |
| `methods_discovery_context.cpp` | Discovery methods | ✅ Context-aware | **KEEP** |
| `methods_auth_context.cpp` | Auth methods | ✅ Context-aware | **KEEP** |
| `methods_multiasset_context.cpp` | Multiasset methods | ✅ Context-aware | **KEEP** |
| `methods_hardware_wallet_context.cpp` | Hardware wallet | ✅ Context-aware | **KEEP** |
| `methods_contract_context.cpp` | Contract methods | ✅ Context-aware | **KEEP** |
| `methods_remaining_context.cpp` | Remaining methods | ✅ Context-aware | **KEEP** |

**Total: 17+ context-aware files** ✅

---

### Category 2: **REVIEW → LIKELY DELETE** - Legacy Handlers (Use Globals)

These files use `extern g_chain_db_direct`, `extern g_wallet_manager`, etc. and should be **deleted** once we confirm context-aware versions exist.

| File | Global Dependencies | Replacement | Action |
|------|-------------------|-------------|--------|
| `methods_blockchain_legacy.cpp` | `g_chain_db_direct` | `methods_blockchain_context.cpp` | **DELETE** |
| `methods_blockchain_legacy.cpp.pre_vnext` | `g_chain_db_direct` | (backup file) | **DELETE** |
| `methods_wallet_legacy.cpp` | `g_wallet_manager` | `methods_wallet_context.cpp` | **DELETE** (if exists) |
| `methods_wallet_legacy.cpp.backup` | `g_wallet_manager` | (backup file) | **DELETE** |
| `methods_mining.cpp` | `g_chain_db_direct`, `g_wallet_manager` | `methods_mining_context.cpp` | **REVIEW** |
| `methods_economics.cpp` | `g_chain_db_direct` | `methods_economics_context.cpp` | **REVIEW** |
| `methods_consensus.cpp` | `g_chain_db_direct` | `methods_consensus_context.cpp` | **REVIEW** |
| `methods_contract.cpp` | `g_chain_db_direct`, `g_wallet_manager` | `methods_contract_context.cpp` | **REVIEW** |
| `methods_wallet.cpp` | Multiple wallet globals | `methods_wallet_context.cpp` | **REVIEW** |
| `methods_p2p.cpp` | `g_wallet_manager` | `methods_network_context.cpp` | **REVIEW** |

**Review Process**: For each file:
1. Check if context-aware version exists
2. Compare method coverage
3. If 100% covered → DELETE
4. If partial coverage → MIGRATE remaining methods to context version

---

### Category 3: **REVIEW → CONSOLIDATE** - Old RPC Handlers (Pre-Refactor)

These are the original RPC handler files before the modular refactor. They may have different patterns.

| File | Status | Action |
|------|--------|--------|
| `blockchain_rpc_handlers.cpp` | Pre-refactor implementation | **REVIEW** |
| `blockchain_rpc_handlers.cpp.pre_vnext` | Backup | **DELETE** |
| `wallet_query_rpc_handlers.cpp` | Pre-refactor wallet queries | **REVIEW** |
| `wallet_legacy_rpc_handlers.cpp` | Pre-refactor wallet | **REVIEW** |
| `wallet_security_rpc_handlers.cpp` | Pre-refactor wallet security | **REVIEW** |
| `mining_rpc_handlers.cpp` | Pre-refactor mining | **REVIEW** |
| `mining_control_rpc_handlers.cpp` | Pre-refactor mining control | **REVIEW** |
| `mining_template_rpc_handlers.cpp` | Pre-refactor mining templates | **REVIEW** |
| `mempool_rpc_handlers.cpp` | Pre-refactor mempool | **REVIEW** |
| `mempool_rpc_handlers.cpp.bak` | Backup | **DELETE** |
| `network_rpc_handlers.cpp` | Pre-refactor network | **REVIEW** |
| `p2p_rpc_handlers.cpp` | Pre-refactor P2P | **REVIEW** |
| `tx_send_rpc_handlers.cpp` | Pre-refactor tx sending | **REVIEW** |
| `storage_rpc_handlers.cpp` | Pre-refactor storage queries | **REVIEW** |
| `storage_info_rpc_handlers.cpp` | Pre-refactor storage info | **REVIEW** |
| `consensus_rpc_handlers.cpp` | Pre-refactor consensus | **REVIEW** |
| `diagnostics_rpc_handlers.cpp` | Diagnostics/debug methods | **KEEP?** |
| `coinjoin_rpc_handlers.cpp` | CoinJoin privacy methods | **KEEP?** |

**Review Process**:
1. Check if methods are covered by context-aware OR legacy files
2. If redundant → **DELETE**
3. If unique functionality → **MIGRATE** to appropriate context file

---

### Category 4: **KEEP** - Infrastructure & Utilities

These provide framework/infrastructure and should be retained.

| File | Purpose | Status |
|------|---------|--------|
| `rpc_registry.cpp` | RPC method registry | **KEEP** ✅ |
| `rpc_startup.cpp` | RPC initialization | **KEEP** ✅ |
| `rpc_introspection.cpp` | Method introspection | **KEEP** ✅ |
| `rpc_meta.cpp` | RPC metadata | **KEEP** ✅ |
| `rpc_adapter.cpp` | RPC adapters | **KEEP** ✅ |
| `legacy_compat.cpp` | Legacy compatibility layer | **KEEP** ✅ |
| `legacy_rpc_adapter.cpp` | Legacy RPC adapter | **KEEP** ✅ |
| `openrpc_generator.cpp` | OpenRPC spec generation | **KEEP** ✅ |
| `address_validation.cpp` | Address validation helpers | **KEEP** ✅ |
| `auth_manager.cpp` | Authentication manager | **KEEP** ✅ |
| `auth_resolver.cpp` | Auth resolver | **KEEP** ✅ |
| `session_manager.cpp` | Session management | **KEEP** ✅ |
| `token_manager.cpp` | Token management | **KEEP** ✅ |

---

### Category 5: **KEEP** - Specialized Features

These implement specific features and should be retained.

| File | Purpose | Status |
|------|---------|--------|
| `event_bus.cpp` | Event bus for subscriptions | **KEEP** ✅ |
| `websocket_event_bridge.cpp` | WebSocket event bridge | **KEEP** ✅ |
| `websocket_server_adapter.cpp` | WebSocket adapter | **KEEP** ✅ |
| `streaming_rpc_handler.cpp` | Streaming RPC support | **KEEP** ✅ |
| `payment_monitor.cpp` | Payment monitoring | **KEEP** ✅ |
| `rpc_mining_events.cpp` | Mining event handlers | **KEEP** ✅ |
| `rpc_health_stubs.cpp` | Health check stubs | **KEEP** ✅ |
| `methods_payjoin.cpp` | PayJoin implementation | **KEEP** ✅ |
| `methods_silent_payments.cpp` | Silent payments | **KEEP** ✅ |
| `methods_openrpc.cpp` | OpenRPC methods | **KEEP** ✅ |
| `methods_marketplace_enhanced.cpp` | Marketplace | **KEEP** ✅ |

---

### Category 6: **DELETE** - Backup Files

| File | Reason |
|------|--------|
| `*.bak`, `*.bak[1-9]` | Backup files |
| `*.backup` | Backup files |
| `*.pre_vnext` | Pre-refactor backups |
| `*.extern` | Experimental |
| `*.final` | Backup |
| `*.vnext_backup` | Backup |

**Action**: Delete all backup files (use git history instead)

**Examples**:
- `methods_economics.cpp.bak`
- `methods_economics.cpp.pre_vnext`
- `methods_mining_extras.cpp.bak[1-7]`
- `methods_wallet_legacy.cpp.backup`
- `mempool_rpc_handlers.cpp.bak`
- `blockchain_rpc_handlers.cpp.pre_vnext`

---

### Category 7: **REVIEW** - VNext Files

These are supposedly the "next generation" implementations. Need to check if they're context-aware.

| File | Status | Action |
|------|--------|--------|
| `methods_wallet_vnext.cpp` | vNext wallet | **CHECK** if context-aware |
| `methods_blockchain_vnext.cpp` | vNext blockchain | **CHECK** if context-aware |
| `methods_mining_vnext.cpp` | vNext mining | **CHECK** if context-aware |
| `methods_network_vnext.cpp` | vNext network | **CHECK** if context-aware |
| `methods_economics_vnext.cpp` | vNext economics | **CHECK** if context-aware |
| `methods_mempool_vnext.cpp` | vNext mempool | **CHECK** if context-aware |
| `methods_sync_vnext.cpp` | vNext sync | **CHECK** if context-aware |
| `methods_auth_vnext.cpp` | vNext auth | **CHECK** if context-aware |
| `methods_consensus_vnext.cpp` | vNext consensus | **CHECK** if context-aware |
| `methods_telemetry_vnext.cpp` | vNext telemetry | **CHECK** if context-aware |
| And many more vnext files... | | |

**Decision Tree**:
- If VNext = Context-aware → **KEEP** (maybe rename without `_vnext`)
- If VNext = Just refactored but still uses globals → **DELETE** (use `_context` version)
- If VNext != Context files → **CONSOLIDATE** into context versions

---

## 🎯 Cleanup Action Plan

### Phase 1: Safe Deletions (No Risk)
**Goal**: Remove obvious redundant files

**Files to Delete** (41+ files):
```bash
# Backup files
rm src/rpc/*.bak*
rm src/rpc/*.backup
rm src/rpc/*.pre_vnext
rm src/rpc/*.extern
rm src/rpc/*.final
rm src/rpc/*.vnext_backup

# List for reference:
methods_economics.cpp.bak
methods_economics.cpp.pre_vnext
methods_mining_extras.cpp.bak*  # (1-7)
methods_wallet_legacy.cpp.backup
methods_mempool.cpp.pre_vnext
methods_blockchain_legacy.cpp.pre_vnext
blockchain_rpc_handlers.cpp.pre_vnext
mempool_rpc_handlers.cpp.bak
websocket_event_bridge.cpp.pre_vnext
methods_wallet.cpp.pre_vnext
methods_network.cpp.pre_vnext
methods_sync_vnext.cpp.pre_vnext
methods_network_vnext.cpp.pre_vnext
methods_economics_vnext.cpp.pre_vnext
methods_telemetry.cpp.pre_vnext
# And more...
```

**Risk**: Zero (these are backup files, not active code)

### Phase 2: Verify Context-Aware Coverage
**Goal**: Confirm context-aware handlers cover all legacy functionality

**Process**:
1. List all methods in `methods_blockchain_legacy.cpp`
2. Verify each exists in `methods_blockchain_context.cpp`
3. Repeat for wallet, mining, mempool, network, economics
4. If 100% coverage → Mark legacy for deletion
5. If partial → Identify missing methods and migrate them first

**Script** (pseudo-code):
```bash
# Extract method names from legacy file
grep "din::Json rpc_legacy_" src/rpc/methods_blockchain_legacy.cpp | \
  sed 's/.*rpc_legacy_\([a-z_]*\).*/\1/' > /tmp/legacy_methods.txt

# Extract method names from context file
grep "din::Json rpc_context_" src/rpc/methods_blockchain_context.cpp | \
  sed 's/.*rpc_context_\([a-z_]*\).*/\1/' > /tmp/context_methods.txt

# Find methods in legacy but not in context
comm -23 /tmp/legacy_methods.txt /tmp/context_methods.txt
```

### Phase 3: Delete Legacy Handlers
**Goal**: Remove files that use globals

**Confirmed Deletions** (pending Phase 2 verification):
```bash
# Legacy handlers (use globals)
src/rpc/methods_blockchain_legacy.cpp        # Replaced by methods_blockchain_context.cpp
src/rpc/methods_blockchain_legacy.h          # Header (if exists)

# Wait for verification before deleting these:
src/rpc/methods_mining.cpp                   # Check if fully covered by methods_mining_context.cpp
src/rpc/methods_economics.cpp                # Check if fully covered by methods_economics_context.cpp
src/rpc/methods_consensus.cpp                # Check if fully covered by methods_consensus_context.cpp
src/rpc/methods_wallet.cpp                   # Check if fully covered by methods_wallet_context.cpp
src/rpc/methods_contract.cpp                 # Check if fully covered by methods_contract_context.cpp
src/rpc/methods_p2p.cpp                      # Check if fully covered by methods_network_context.cpp
```

### Phase 4: Consolidate Old RPC Handlers
**Goal**: Migrate unique functionality from pre-refactor files

**Process**:
1. Review each `*_rpc_handlers.cpp` file
2. Check if methods are covered by `methods_*_context.cpp`
3. If unique functionality exists → Migrate to context version
4. Delete old handler file

**Files to Review**:
```bash
src/rpc/blockchain_rpc_handlers.cpp
src/rpc/wallet_query_rpc_handlers.cpp
src/rpc/wallet_legacy_rpc_handlers.cpp
src/rpc/wallet_security_rpc_handlers.cpp
src/rpc/mining_rpc_handlers.cpp
src/rpc/mining_control_rpc_handlers.cpp
src/rpc/mining_template_rpc_handlers.cpp
src/rpc/mempool_rpc_handlers.cpp
src/rpc/network_rpc_handlers.cpp
src/rpc/p2p_rpc_handlers.cpp
src/rpc/tx_send_rpc_handlers.cpp
src/rpc/storage_rpc_handlers.cpp
src/rpc/storage_info_rpc_handlers.cpp
src/rpc/consensus_rpc_handlers.cpp
```

### Phase 5: Review VNext Files
**Goal**: Determine if vnext = context or if vnext is redundant

**Process**:
```bash
# Check if vnext files use ctx.daemon pattern
grep "ctx.daemon" src/rpc/methods_*_vnext.cpp

# If yes → These are context-aware, possibly rename
# If no → These are just refactored, check if context version exists
```

**Possible Actions**:
- If vnext IS context-aware → Rename to `*_context.cpp` for consistency
- If vnext IS NOT context-aware → Migrate to context pattern or delete if redundant

### Phase 6: Remove Bridge Globals
**Goal**: Force all code to use context pattern

**File**: `src/daemon/legacy_globals_stub.cpp`

**Process**:
1. Comment out all global variable definitions
2. Build and check for errors
3. Fix any remaining global usages
4. Delete the file entirely

**Expected Errors**: Non-RPC code that still uses globals (block_acceptor, mining_safety_gates, etc.)

**Fix Strategy**: Update those files to receive DaemonContext via constructor/method parameter

---

## 📊 Estimated Impact

### Files to Delete (Conservative Estimate)
- **Backup files**: ~40 files
- **Legacy handlers**: ~10 files
- **Old RPC handlers**: ~15 files (if redundant)
- **VNext duplicates**: ~10 files (if redundant)

**Total**: ~75 files deleted (out of ~140 RPC-related files)

### Lines of Code Removed
- **Backup files**: ~30,000 lines (duplicate code)
- **Legacy handlers**: ~5,000 lines (replaced by context-aware)
- **Old handlers**: ~8,000 lines (if consolidated)

**Total**: ~43,000 lines removed

### Remaining Codebase
- **Context-aware handlers**: ~20,000 lines (modern, clean)
- **Infrastructure**: ~5,000 lines (registry, auth, adapters)
- **Specialized features**: ~8,000 lines (WebSocket, events, etc.)

**Total**: ~33,000 lines (focused, maintainable)

---

## ✅ Verification Checklist

After cleanup, verify:

- [ ] All backup files deleted
- [ ] No references to deleted files in CMakeLists.txt
- [ ] Context-aware handlers registered correctly
- [ ] All RPC methods still accessible
- [ ] No compilation errors
- [ ] No global variable references in RPC handlers
- [ ] `g_rpcRegistry.methodNames()` returns expected count
- [ ] Test suite passes
- [ ] Mainnet daemon starts and handles RPC calls

---

## 🚀 Execution Order

**Week 1: Analysis**
1. Complete Phase 2 (verify coverage)
2. Create final deletion list
3. Backup current state (git commit)

**Week 1: Safe Cleanup**
4. Execute Phase 1 (delete backups)
5. Build and test (should pass)
6. Commit cleanup

**Week 2: Legacy Removal**
7. Execute Phase 3 (delete legacy handlers)
8. Fix any build errors
9. Test all RPC methods
10. Commit changes

**Week 2: Consolidation**
11. Execute Phase 4 (consolidate old handlers)
12. Migrate unique functionality
13. Delete redundant files
14. Test and commit

**Week 3: Global Removal**
15. Execute Phase 6 (remove bridge globals)
16. Fix non-RPC global usages
17. Final testing
18. Document completion

---

## 📝 Next Action

**Immediate**: Execute Phase 2 to verify context-aware handler coverage

Would you like me to:
1. Create the method coverage verification script?
2. Proceed with Phase 1 (delete backup files)?
3. Analyze specific files in detail?

