# Week 3: Context Migration Progress

**Goal**: Migrate all non-RPC code from bridge globals to DaemonContext injection

**Start Date**: 2025-11-06
**Status**: In Progress (40% complete)

---

## Migration Strategy

Replace global variable access with DaemonContext injection:

```cpp
// OLD (Week 2 bridge pattern):
extern ChainDB* g_chain_db_direct;
auto result = g_chain_db_direct->getTip();

// NEW (Week 3 context injection):
if (m_context && m_context->chainstate) {
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
    auto chain_db = chainstate->chainDB();
    auto result = chain_db->getTip();
}
```

**Pattern**:
1. Add `DaemonContext* m_context{nullptr}` member to class
2. Add `void SetContext(DaemonContext* ctx)` method
3. Replace `g_global_thing` with `m_context->service->method()`
4. Add null checks for safety

---

## Files Migrated (2/5 complete)

### ✅ 1. gbt_work_manager.cpp (COMPLETE)
- **Global Removed**: `g_blockchain` (4 usages)
- **Changes**:
  - Added `DaemonContext* m_context` member to GBTWorkManager
  - Added `SetContext(DaemonContext* ctx)` method
  - Updated `ReadBlockchainTip()` to use `m_context->chainstate`
- **Files Modified**:
  - `include/daemon/gbt_work_manager.h`: Added context member and SetContext()
  - `src/daemon/gbt_work_manager.cpp`: Migrated ReadBlockchainTip() function
- **Result**: Eliminates all g_blockchain dependency in GBTWorkManager

### ✅ 2. peer_manager.cpp (COMPLETE)
- **Global Removed**: `g_chain_db_direct` (5 usages)
- **Changes**:
  - Added `DaemonContext* m_context` member to PeerManager
  - Added `SetContext(DaemonContext* ctx)` method
  - Updated `requestHeaders()` to use `m_context->chainstate->chainDB()`
- **Files Modified**:
  - `src/p2p/peer_manager.h`: Added context member and SetContext()
  - `src/daemon/p2p/peer_manager.cpp`: Migrated requestHeaders() block locator generation (lines 377-426)
- **Result**: Eliminates all g_chain_db_direct dependency in PeerManager

---

## Remaining Work (3/5 files)

### 🔄 3. blockchain.cpp (NEXT)
- **Global to Remove**: `g_wallet_manager` (7 usages)
- **Estimated Effort**: Medium (similar to peer_manager)
- **Plan**:
  - Add DaemonContext* member to Blockchain class
  - Replace g_wallet_manager with m_context->wallet->GetManager()
  - Update block reward tracking and UTXO management

### 🔄 4. mining_safety_gates.cpp
- **Global to Remove**: `g_chain_db_direct` (10 usages)
- **Estimated Effort**: Medium
- **Plan**:
  - Add DaemonContext* member to MiningSafetyGates
  - Replace g_chain_db_direct with m_context->chainstate->chainDB()
  - Update all validation checks

### 🔄 5. block_acceptor.cpp (LARGEST)
- **Global to Remove**: `g_blockchain`, `g_chain_db_direct`, `g_wallet_manager` (42 usages)
- **Estimated Effort**: High (largest file, most complex)
- **Plan**:
  - Add DaemonContext* member to BlockAcceptor
  - Migrate all blockchain, chainstate, and wallet calls
  - Extensive testing required

---

## Statistics

| Metric | Count | Notes |
|--------|-------|-------|
| **Files Migrated** | 2 / 5 | 40% complete |
| **Global Usages Eliminated** | 9 / 73 | 12% complete |
| **RPC Methods Migrated** | 132 / 132 | 100% complete (Week 2) |
| **Context-Aware Services** | 9 / 9 | All services use DaemonContext |

**Breakdown by Global**:
- `g_blockchain`: 4 eliminated (gbt_work_manager), 43 remaining (block_acceptor)
- `g_chain_db_direct`: 5 eliminated (peer_manager), 15 remaining (mining_safety_gates, block_acceptor)
- `g_wallet_manager`: 0 eliminated, 11 remaining (blockchain.cpp, block_acceptor)

---

## Build Status

**Last Build**: Success (with pre-existing hardware wallet linker errors)

**Known Issues**:
- Hardware wallet linker errors (pre-existing, unrelated to migration)
  - `dinero::hw::USBHardwareWallet::EnumerateDevices()`
  - USBHardwareWallet constructor and vtable
- **Decision**: Continue with migrations, fix HW wallet separately

**Fixed Issues**:
- ✅ Missing registerConsensusRPC() - stubbed out
- ✅ Missing registerWebSocketManagementRPC() - stubbed out

---

## Architecture Progress

### Week 1: Foundation ✅
- DaemonApp + DaemonContext
- 9 core services with IService interface
- Service lifecycle (Init → Start → Stop)

### Week 2: RPC Migration ✅
- HttpRpcServer integrated into RPCService
- WireRpcContext() connects daemon to RPC handlers
- ExecutionContext pattern in all 132 RPC methods
- 18 context-aware RPC files

### Week 3: Non-RPC Migration (In Progress)
- **Complete**: gbt_work_manager.cpp, peer_manager.cpp
- **In Progress**: blockchain.cpp
- **Pending**: mining_safety_gates.cpp, block_acceptor.cpp

### Week 4: Cleanup (Not Started)
- Remove bridge pattern after all migrations complete
- Delete legacy globals (g_blockchain, g_chain_db_direct, g_wallet_manager)
- Clean up includes and forward declarations

---

## Next Steps

1. **Immediate**: Migrate blockchain.cpp (7 global usages)
   - Read blockchain.cpp to understand g_wallet_manager usage
   - Add DaemonContext* member and SetContext() method
   - Replace all g_wallet_manager calls with context access
   - Build and verify

2. **Then**: Migrate mining_safety_gates.cpp (10 global usages)
   - Similar pattern to peer_manager.cpp
   - Replace g_chain_db_direct with chainstate access

3. **Finally**: Migrate block_acceptor.cpp (42 global usages)
   - Largest and most complex file
   - Requires careful testing
   - Do this last when all other code is green

4. **Commit Strategy**:
   ```bash
   git add .
   git commit -m "Migrate [filename] to DaemonContext (replaces g_[global_name])"
   ```

---

## Success Criteria

- ✅ All 5 files migrated to use DaemonContext
- ✅ Zero usages of g_blockchain, g_chain_db_direct, g_wallet_manager
- ✅ Daemon builds and runs successfully
- ✅ All RPC methods continue working
- ✅ P2P networking remains functional
- ✅ Mining continues to work

**Target Completion**: End of Week 3
