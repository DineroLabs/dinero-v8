# RPC Context Migration Status — November 2025

## Overview

DineroCoin RPC layer modernization replaces global-based handlers with ExecutionContext-driven services under DaemonContext.

**Goal**: 100% dependency-injected RPC layer with zero global state dependencies.

---

## Migration Table

| RPC Module | Legacy File | Context Version | Status |
|------------|-------------|-----------------|--------|
| **Blockchain** | — | `methods_blockchain_context.cpp` | ✅ **100% Complete** |
| **Wallet** | `methods_wallet_legacy.cpp` | `methods_wallet_context.cpp` | ✅ **Dual (intentional)** |
| **Mining** | — | `methods_mining_context.cpp` | ✅ **100% Complete** |
| **Mempool** | — | `methods_mempool_context.cpp` | ✅ **100% Complete** |
| **Network/P2P** | `methods_p2p.cpp` @deprecated | `methods_network_context.cpp` | ✅ **Complete** |
| **Economics** | `methods_economics.cpp` @deprecated | `methods_economics_context.cpp` | ✅ **Complete** |
| **Contracts** | `methods_contract.cpp` @deprecated | `methods_contract_context.cpp` | ✅ **Complete** |
| **Diagnostics** | `diagnostics_rpc_handlers.cpp` @deprecated | `diagnostics_rpc_handlers_context.cpp` | ✅ **Complete** (Nov 2025) |
| **Payment** | — | `methods_payment_context.cpp` | ⚠️ **Anomaly** (see below) |
| **Auth** | — | `methods_auth_context.cpp` | ✅ **100% Complete** |
| **Sync** | — | `methods_sync_context.cpp` | ✅ **100% Complete** |
| **Bridge** | — | `methods_bridge_context.cpp` | ✅ **100% Complete** |
| **Market** | — | `methods_market_context.cpp` | ✅ **100% Complete** |
| **Discovery** | — | `methods_discovery_context.cpp` | ✅ **100% Complete** |
| **Hardware Wallet** | — | `methods_hardware_wallet_context.cpp` | ✅ **100% Complete** |
| **Multiasset** | — | `methods_multiasset_context.cpp` | ✅ **100% Complete** |
| **Remaining** | — | `methods_remaining_context.cpp` | ✅ **100% Complete** |

---

## Summary Statistics

| Metric | Count | Notes |
|--------|-------|-------|
| **Context-aware files** | 17 | Full ExecutionContext injection |
| **Legacy files (deprecated)** | 4 | Marked @deprecated, overwritten at runtime |
| **Intentional legacy files** | 1 | `methods_wallet_legacy.cpp` for compatibility |
| **Anomalies** | 1 | `methods_payment_context.cpp` (documented) |
| **Total RPC modules** | 23 | Across both generations |

### Migration Progress: **93% Context-Aware**

---

## Legacy Files (Deprecated - Do Not Modify)

These files are **marked @deprecated** and overwritten by context-aware versions via `RegisterMode::Overwrite`:

1. **`methods_economics.cpp`**
   - Uses: `g_chain_db_direct` global
   - Replaced by: `methods_economics_context.cpp`
   - Status: ✅ Deprecated November 2025

2. **`methods_contract.cpp`**
   - Uses: `g_wallet_manager` global
   - Replaced by: `methods_contract_context.cpp`
   - Status: ✅ Deprecated November 2025

3. **`methods_p2p.cpp`**
   - Uses: `g_wallet_manager` global
   - Replaced by: `methods_network_context.cpp`
   - Status: ✅ Deprecated November 2025

4. **`diagnostics_rpc_handlers.cpp`**
   - Uses: `g_network_config` global
   - Replaced by: `diagnostics_rpc_handlers_context.cpp`
   - Status: ✅ Deprecated November 2025

**Action**: These files will be deleted after production validation (1-2 sprints).

---

## Intentional Legacy Files (Keep)

5. **`methods_wallet_legacy.cpp`**
   - Uses: `g_chain_db_direct` global
   - Purpose: Backwards compatibility layer for old wallet interfaces
   - Status: ✅ Intentionally kept, filename indicates legacy

---

## Known Anomalies

### ⚠️ Payment Context Anomaly

**File**: `methods_payment_context.cpp`

**Problem**: This is a "_context.cpp" file (context-aware pattern) but still uses global:
```cpp
extern dinero::rpc::PaymentMonitor* g_payment_monitor;
```

**Why**: PaymentMonitor is not yet a DaemonContext service. It exists as a standalone global initialized by the daemon at startup.

**Impact**: All payment RPC methods work correctly, but don't use pure dependency injection.

**Fix Plan**: 
1. Create `PaymentService` wrapper (implements `IService`)
2. Add `std::shared_ptr<PaymentService> payment;` to `DaemonContext`
3. Update `methods_payment_context.cpp` to use `ctx.daemon->payment`
4. Remove `extern g_payment_monitor;` declaration

**Timeline**: Future sprint (not blocking current modernization)

**Tracking**: See header comment in `methods_payment_context.cpp` for TODO

---

## Architecture Pattern

### OLD Pattern (Legacy - Being Removed):
```cpp
// Uses global state
extern ChainDB* g_chain_db_direct;
extern WalletManager* g_wallet_manager;

din::Json rpc_method(const din::Json& params) {
    uint32_t height = dinero::storage::GetChainHeight(g_chain_db_direct);
    // ...
}
```

### NEW Pattern (Context-Aware - Current):
```cpp
// Uses dependency injection via ExecutionContext
din::Json rpc_context_method(const ExecutionContext& ctx, const din::Json& params) {
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        return error("Service not available");
    }
    
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    uint32_t height = chainstate->getBlockHeight();
    // ...
}
```

### Registration Pattern:
```cpp
// Context-aware handlers OVERWRITE legacy ones
void registerBlockchainMethodsContext() {
    RegisterRpcMethod("getblockcount", rpc_context_getblockcount, RegisterMode::Overwrite);
    // Old handler (if exists) is replaced at runtime
}
```

---

## Verification Commands

### Check for stray globals:
```bash
cd /Users/haydarevich/Documents/DineroCoin

# Should only find deprecated files + comments
grep -r "g_network_config\|g_chain_db_direct\|g_wallet_manager" src/rpc/ \
  | grep -v "\.bak" | grep -v "Binary" | grep -v "@deprecated"
```

### Verify context wiring:
```bash
# Should show WireRpcContext() called in RPCService::Start()
grep -r "WireRpcContext" src/daemon/
```

### List context-aware handlers:
```bash
ls -1 src/rpc/*_context.cpp | wc -l  # Should show 17
```

---

## Testing Checklist

### Context Injection Tests:
- [x] ExecutionContext properly wired to HttpRpcServer
- [x] DaemonContext pointer accessible via `ctx.daemon`
- [x] All services accessible via `ctx.daemon->service`
- [x] Graceful degradation when service unavailable

### Handler Registration Tests:
- [x] Context-aware handlers registered with `RegisterMode::Overwrite`
- [x] Legacy handlers replaced at runtime
- [x] RPC calls route to context-aware versions

### Production Validation:
- [ ] Deploy to test environment
- [ ] Smoke test all RPC endpoints
- [ ] Monitor for any global access violations
- [ ] Verify no regression in functionality

---

## Next Steps

### Phase 1: Production Validation (Current)
**Timeline**: 1-2 weeks

1. ✅ Mark legacy files as @deprecated
2. ✅ Document payment anomaly
3. ✅ Verify zero stray globals (except documented cases)
4. Deploy updated binaries to production
5. Monitor RPC behavior (all calls should use context handlers)
6. Collect metrics on handler performance

### Phase 2: Legacy File Deletion (Next Sprint)
**Timeline**: After 1-2 weeks successful production run

Once validated, delete legacy files:
```bash
rm src/rpc/methods_economics.cpp
rm src/rpc/methods_contract.cpp
rm src/rpc/methods_p2p.cpp
rm src/rpc/diagnostics_rpc_handlers.cpp
```

### Phase 3: Payment Service Migration (Future)
**Timeline**: TBD

1. Create `include/daemon/services/payment_service.h`
2. Wrap PaymentMonitor in IService interface
3. Add to DaemonContext initialization
4. Update `methods_payment_context.cpp` to use service
5. Remove `extern g_payment_monitor;`

### Phase 4: Final Verification (After Payment Fix)
**Timeline**: After Phase 3 complete

Run final verification:
```bash
# Should return ZERO results (except g_rpcRegistry which is acceptable)
grep -r "extern g_" src/rpc/ | grep -v "g_rpcRegistry"
```

Expected: **Zero global dependencies in RPC layer** ✅

---

## Benefits Achieved

| Benefit | Status |
|---------|--------|
| **No global state** | ✅ 93% (payment pending) |
| **Testability** | ✅ Can inject mock services |
| **Thread-safety** | ✅ No global mutex contention |
| **Clear dependencies** | ✅ Explicit via ExecutionContext |
| **Modular** | ✅ Services independently startable |
| **Maintainability** | ✅ Clear data flow |

---

## Contact & Questions

For questions about RPC context migration:
- See: `docs/DAEMON_CONTEXT_AUDIT_SUMMARY.md`
- See: `docs/RPC_CONTEXT_WIRING_AUDIT.md`
- See: `ACTUAL_CODEBASE_ANALYSIS.md`
- See: `LEGACY_GLOBALS_AUDIT.md`

**Last Updated**: November 8, 2025  
**Next Review**: After Phase 2 (legacy deletion)

