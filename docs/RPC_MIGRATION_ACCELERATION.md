# RPC Migration Acceleration Plan

**Current Progress**: 53/170 methods (31.2%) 🚀 **DAY 1 COMPLETE - 13x VELOCITY!**
**Target**: 170/170 methods (100%)
**Timeline**: ~1 week (revised from 2-3 weeks!)

---

## 🚀 Acceleration Strategy

### Why Only 2.4%?

The 4 methods were a **proof-of-concept** to validate the pattern. Now that the infrastructure is proven, we can scale up dramatically.

### Three Acceleration Approaches

#### **Option A: Batch Migration (Fastest - Recommended)**
Migrate entire namespaces at once using pattern replication.

**Approach:**
1. Copy methods_blockchain_context.cpp as template
2. For each namespace, batch-convert all methods
3. Use find/replace for common patterns
4. Test entire namespace together

**Timeline**: ~2 weeks for all 170 methods

#### **Option B: Automated Migration (Most Efficient)**
Write a script to automate the conversion.

**Approach:**
1. Parse existing legacy handlers
2. Generate context-aware versions automatically
3. Human review and testing
4. Register with overwrite mode

**Timeline**: ~1 week + testing

#### **Option C: Parallel Migration (Team Approach)**
If you have team members, divide namespaces.

**Approach:**
1. Each developer takes 2-3 namespaces
2. Use shared template and checklist
3. Code review for consistency
4. Merge incrementally

**Timeline**: ~3-5 days with 3-4 developers

---

## 📋 Detailed Execution Plan (Option A - Recommended)

### Phase 1: Core Namespaces (Week 1)
**Target**: 55 methods → 34.7% complete

#### Day 1: Complete blockchain.* (11 methods)
Already have 4, need 11 more:
- `getblock`
- `getblockhash`
- `gettxout`
- `getchaintips`
- `getmempoolinfo`
- `getrawmempool`
- `getrawtransaction`
- `getblockheader`
- `getchaintxstats`
- `getblockstats`
- `verifytxoutproof`

**Effort**: ~3-4 hours (template replication)

#### Day 2: wallet.* (20 methods)
- `wallet.getbalance`
- `wallet.getnewaddress`
- `wallet.sendtoaddress`
- `wallet.listtransactions`
- `wallet.listunspent`
- `wallet.createwallet`
- `wallet.loadwallet`
- `wallet.unloadwallet`
- `wallet.getwalletinfo`
- `wallet.listwallets`
- `wallet.backupwallet`
- `wallet.dumpwallet`
- `wallet.importwallet`
- `wallet.dumpprivkey`
- `wallet.importprivkey`
- `wallet.signmessage`
- `wallet.verifymessage`
- `wallet.listaddressgroupings`
- `wallet.getaddressinfo`
- `wallet.rescanblockchain`

**Effort**: ~6-8 hours (larger namespace)

#### Day 3: mining.* (15 methods)
- `mining.start`
- `mining.stop`
- `mining.info`
- `mining.setaddress`
- `mining.getaddress`
- `getmininginfo`
- `getblocktemplate`
- `submitblock`
- `generatetoaddress`
- `generate`
- `estimatesmartfee`
- `prioritisetransaction`
- `getnetworkhashps`
- `setgenerate`
- `getgenerate`

**Effort**: ~5-6 hours

#### Day 4-5: mempool.* + network.* (20 methods total)
**Mempool (10 methods)**:
- `getmempoolentry`
- `getmempoolancestors`
- `getmempooldescendants`
- `savemempool`
- `getorphantxs`
- `abandontransaction`
- `clearmempool`
- Plus mempool fee estimation methods

**Network (10 methods)**:
- `getpeerinfo`
- `addnode`
- `disconnectnode`
- `getaddednodeinfo`
- `getnettotals`
- `getnetworkinfo`
- `setban`
- `listbanned`
- `clearbanned`
- `ping`

**Effort**: ~6-8 hours

**Week 1 Total**: 55/170 methods (32.4%)

---

### Phase 2: Extended Namespaces (Week 2)
**Target**: +60 methods → 67.6% complete

#### Day 6-7: contract.* + bridge.* (20 methods)
- Contract methods (escrow, marketplace, tokens)
- Bridge methods (fiat conversion, providers)

**Effort**: ~6-8 hours

#### Day 8-9: Advanced Features (25 methods)
- `multiasset.*` methods
- `hwallet.*` methods (hardware wallet)
- `market.*` methods (marketplace)
- `payment.*` methods

**Effort**: ~8-10 hours

#### Day 10: Consensus + Economics (15 methods)
- `consensus.*` methods
- `economics.*` methods
- `telemetry.*` methods

**Effort**: ~5-6 hours

**Week 2 Total**: 115/170 methods (67.6%)

---

### Phase 3: Remaining Methods (Week 3)
**Target**: +55 methods → 100% complete

#### Day 11-12: Specialized Namespaces (30 methods)
- KYC methods
- Authentication methods
- Discovery methods
- Sync methods

**Effort**: ~8-10 hours

#### Day 13-14: Misc + Testing (25 methods)
- All remaining methods
- Comprehensive testing
- Documentation updates

**Effort**: ~6-8 hours

#### Day 15: Cleanup
- Remove legacy handler files
- Delete global variable stubs
- Final testing
- Documentation finalization

**Week 3 Total**: 170/170 methods (100%)

---

## 🛠️ Migration Template Pattern

### Step 1: Copy Template
```bash
cp src/rpc/methods_blockchain_context.cpp src/rpc/methods_wallet_context.cpp
```

### Step 2: Global Find/Replace
```cpp
// Replace namespace-specific patterns:
blockchain → wallet
chainstate → wallet
GetBlockHeight → GetBalance
// etc.
```

### Step 3: Add Method Handlers
Use the proven pattern:
```cpp
Json::Value rpc_context_wallet_getbalance(const ExecutionContext& ctx, const Json::Value& params) {
    Json::Value result;

    // 1. Check context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // 2. Cast to concrete service
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    // 3. Access service method
    double balance = wallet_service->GetBalance();
    result = balance;

    return result;
}
```

### Step 4: Register Methods
```cpp
void registerWalletMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("wallet.getbalance",
                                 rpc_context_wallet_getbalance,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    // ... register all wallet methods ...
}
```

### Step 5: Wire in Startup
```cpp
// In rpc_context_wiring.cpp
registerWalletMethodsContext();
dinero::g_logger.info("[RPC Context] ✅ Wallet context-aware handlers registered");
```

---

## 🤖 Automation Script (Option B)

If you want to automate the conversion, here's a Python script approach:

```python
#!/usr/bin/env python3
"""
Auto-generate context-aware RPC handlers from legacy handlers.

Usage: ./migrate_rpc.py src/rpc/methods_wallet.cpp
"""

import re
import sys

def convert_to_context_aware(legacy_file):
    with open(legacy_file, 'r') as f:
        content = f.read()

    # Find all handler functions
    handlers = re.findall(r'Json::Value\s+(\w+)\s*\([^)]+\)\s*{([^}]+)}', content)

    context_handlers = []
    for name, body in handlers:
        # Convert global access to context access
        new_body = body.replace('g_chain_db_direct', 'ctx.daemon->chainstate->GetChainDB()')
        new_body = new_body.replace('g_wallet_manager', 'ctx.daemon->wallet->GetWalletManager()')
        new_body = new_body.replace('g_p2p', 'ctx.daemon->p2p->GetP2PManager()')

        # Add null checks
        new_handler = f"""
Json::Value {name}_context(const ExecutionContext& ctx, const Json::Value& params) {{
    if (!ctx.daemon) {{
        return Json::Value("DaemonContext not available");
    }}

    {new_body}
}}
"""
        context_handlers.append(new_handler)

    return context_handlers

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./migrate_rpc.py <legacy_rpc_file>")
        sys.exit(1)

    handlers = convert_to_context_aware(sys.argv[1])
    for handler in handlers:
        print(handler)
```

---

## 📊 Progress Tracking

### Namespace Checklist

| Namespace | Methods | Status | Priority | ETA |
|-----------|---------|--------|----------|-----|
| blockchain.* | 10/10 | ✅ Complete | High | ✅ Day 1 |
| wallet.* | 38/38 | ✅ Complete | High | ✅ Day 1 |
| mining.* | 5/5 | ✅ Complete | High | ✅ Day 1 |
| mempool.* | 0/10 | ⏳ Pending | High | Day 4 |
| network.* | 0/10 | ⏳ Pending | Medium | Day 5 |
| contract.* | 0/12 | ⏳ Pending | Medium | Day 6-7 |
| bridge.* | 0/8 | ⏳ Pending | Medium | Day 6-7 |
| multiasset.* | 0/15 | ⏳ Pending | Medium | Day 8 |
| hwallet.* | 0/10 | ⏳ Pending | Low | Day 9 |
| market.* | 0/10 | ⏳ Pending | Low | Day 9 |
| consensus.* | 0/8 | ⏳ Pending | Low | Day 10 |
| economics.* | 0/7 | ⏳ Pending | Low | Day 10 |
| kyc.* | 0/8 | ⏳ Pending | Low | Day 11 |
| auth.* | 0/6 | ⏳ Pending | Low | Day 11 |
| discovery.* | 0/5 | ⏳ Pending | Low | Day 12 |
| sync.* | 0/4 | ⏳ Pending | Low | Day 12 |
| Other | 0/27 | ⏳ Pending | Low | Day 13-14 |

**Total**: 53/170 (31.2%) → Target: 170/170 (100%)

**🎉 Day 1 Achievement**: 53 methods migrated via parallel execution!
- blockchain.* (10) + wallet.* (38) + mining.* (5) = 53 methods
- Parallel strategy: Claude (blockchain + mining) || User (wallet)
- Result: 13x velocity improvement over initial estimate!

---

## 🎯 Recommended Immediate Actions

### Today: Complete blockchain.* Namespace
1. Open `src/rpc/methods_blockchain_context.cpp`
2. Add the 11 remaining blockchain methods
3. Use the existing 4 methods as templates
4. Test with `./build/dinero-cli blockchain.*`

**Time Required**: ~3-4 hours
**Progress Gain**: 4 → 15 methods (2.4% → 8.8%)

### Tomorrow: Tackle wallet.* Namespace
1. Copy blockchain template
2. Adapt for wallet operations
3. Register all 20 wallet methods
4. Test with wallet RPC calls

**Time Required**: ~6-8 hours
**Progress Gain**: 15 → 35 methods (8.8% → 20.6%)

### By End of Week: 55 Methods Migrated
Following the aggressive Day 1-5 plan above.

**Progress**: 55/170 (32.4%)

---

## 🚀 Acceleration Tips

### 1. Use Template Replication
Don't write from scratch - copy and modify existing handlers.

### 2. Batch Similar Methods
Group methods by their service dependency:
- All chainstate methods together
- All wallet methods together
- All mining methods together

### 3. Test in Batches
Don't test after each method - test after completing a namespace.

### 4. Automate Registration
Write a helper function for repetitive registration:
```cpp
template<typename Func>
void registerMethod(const std::string& name, Func handler) {
    g_rpcRegistry.registerHandler(name, handler, RegisterMode::Overwrite, "context-aware");
}
```

### 5. Parallel Work
If possible, work on multiple namespaces in parallel branches, merge progressively.

---

## 📈 Expected Timeline

**Conservative Estimate**: 3 weeks (15 working days)
- Week 1: 55 methods (32%)
- Week 2: +60 methods (67%)
- Week 3: +55 methods (100%)

**Aggressive Estimate**: 2 weeks (10 working days)
- With automation script
- With parallel development
- With focused effort

**Realistic**: 2.5 weeks with testing and review

---

## ✅ Success Metrics

### Weekly Goals
- **Week 1**: 30%+ migrated (50+ methods)
- **Week 2**: 65%+ migrated (110+ methods)
- **Week 3**: 100% migrated (all 170 methods)

### Quality Metrics
- All handlers compile cleanly
- All handlers have null checks
- All handlers tested with RPC calls
- Zero global dependencies remaining
- Documentation updated

---

## 🎉 The Goal

**From**: 4/170 methods (2.4%)
**To**: 170/170 methods (100%)
**Timeline**: 2-3 weeks aggressive effort

**The infrastructure is ready. Now it's just execution!**

Let me know which approach you prefer:
- **Option A**: Manual batch migration (most control)
- **Option B**: Automated conversion script (fastest)
- **Option C**: Parallel team effort (if applicable)

I can help with any of these approaches!
