# What's Next: Week 2 Quick Start

## ✅ Week 1 Complete!

You now have:
- **Working dinerod binary** (48MB, all services running)
- **Bridge architecture** (services set legacy globals)
- **Clean main.cpp** (113 lines)
- **9 real services** creating actual instances

## 🚀 Week 2 Goals

**Transform the bridge pattern into pure dependency injection**

Remove legacy globals entirely by migrating all code to use DaemonContext.

## Quick Start: Day 1-2

### Option A: Testing Phase
Start by thoroughly testing what we built:

```bash
# Test daemon
cd /Users/haydarevich/Documents/DineroCoin

# Start daemon in test mode
./build/dinerod --regtest --datadir=/tmp/test-week2

# In another terminal, test RPC:
./build/dinero-cli -datadir=/tmp/test-week2 getblockcount
./build/dinero-cli -datadir=/tmp/test-week2 getinfo
./build/dinero-cli -datadir=/tmp/test-week2 rpc.listmethods

# Test that globals are set:
grep "Legacy global.*→" /tmp/test-week2/debug.log
```

### Option B: Start Migration
Begin migrating RPC handlers to use DaemonContext:

**Step 1: Pick one simple RPC handler**
```bash
# Start with a simple one like getblockcount
vim src/rpc/methods_blockchain.cpp
```

**Step 2: Add DaemonContext parameter**
```cpp
// BEFORE:
Json::Value handle_getblockcount(const Json::Value& params) {
    uint32_t height = g_chain_db_direct->GetHeight();
    return Json::Value(height);
}

// AFTER:
Json::Value handle_getblockcount(const Json::Value& params, DaemonContext& ctx) {
    uint32_t height = ctx.chainstate->GetHeight();
    return Json::Value(height);
}
```

**Step 3: Update RPC registration**
```cpp
// RpcRegistry needs to store DaemonContext& and pass it to handlers
```

## Recommended Approach

### This Week (Week 2):

**Day 1-2: Testing & Verification**
- Run daemon for extended periods
- Test all RPC commands
- Check for memory leaks
- Verify globals are set correctly

**Day 3-4: Begin RPC Migration**
- Add DaemonContext to RpcRegistry
- Migrate blockchain RPC handlers
- Migrate wallet RPC handlers
- Keep bridge in place

**Day 5-6: Add Service Accessors**
- Add GetBlockchain() to ChainstateService
- Add GetP2PManager() to P2PService
- Add GetWalletManager() to WalletService

**Day 7: Testing**
- Verify migrated code works
- Prepare for global removal (Week 3)

### Next Week (Week 3):

**Remove all legacy globals**
- Delete bridge assignments from services
- Delete legacy_globals_stub.cpp
- Remove all extern declarations
- Pure service architecture complete!

## Key Files to Know

### Current Architecture
```
docs/
├── BRIDGE_ARCHITECTURE.md         ← How bridge pattern works
├── SERVICE_ARCHITECTURE_REALITY_CHECK.md  ← Why not stubs
├── WEEK1_COMPLETE.md              ← What we accomplished
├── WEEK2_ROADMAP.md               ← Detailed Week 2 plan
└── WHATS_NEXT.md                  ← This file

src/daemon/
├── main.cpp                       ← Clean 113 lines
├── daemon_app.cpp                 ← Creates services
├── daemon_context.h               ← DI container
├── legacy_globals_stub.cpp        ← REMOVE in Week 3
└── services/*.cpp                 ← 9 service implementations
```

### Migration Priority

**High Priority (Week 2)**
1. RPC handlers in `src/rpc/methods_*.cpp`
2. Mining code in `src/daemon/mining.cpp`
3. Block acceptor in `src/daemon/block_acceptor.cpp`

**Low Priority (Week 3+)**
4. Internal service code (already mostly isolated)
5. Remove singleton patterns (EventBus, etc.)

## How to Ask for Help

If you want to start migration, just say:
- "Migrate RPC handlers to use DaemonContext"
- "Add service accessor methods"
- "Remove legacy globals"

If you want to test first:
- "Run comprehensive tests"
- "Check for memory leaks"
- "Verify all services work"

If you want documentation:
- "Document the migration process"
- "Create migration examples"
- "Write testing guide"

## The Vision

**Week 1 (Complete):** Bridge pattern
```cpp
main() → DaemonApp → Services → Set globals
Old code uses g_thing (via bridge to real instances)
```

**Week 2 (In Progress):** Migration
```cpp
main() → DaemonApp → Services
RPC handlers use ctx.service (direct access)
Old code still uses g_thing (bridge still active)
```

**Week 3 (Goal):** Pure DI
```cpp
main() → DaemonApp → Services
All code uses ctx.service (pure dependency injection)
No globals, no bridge - clean architecture! 🎉
```

## Quick Commands

```bash
# Build
cd /Users/haydarevich/Documents/DineroCoin
cmake --build build

# Test daemon
./build/dinerod --regtest --datadir=/tmp/test

# Test RPC
./build/dinero-cli -datadir=/tmp/test getinfo

# Check binary
ls -lh build/dinerod

# View logs
tail -f /tmp/test/debug.log
```

## Success Metrics

Week 2 is complete when:
- [ ] All RPC handlers use DaemonContext (not globals)
- [ ] All mining code uses DaemonContext
- [ ] All P2P code uses DaemonContext
- [ ] Services have accessor methods
- [ ] Everything still works!
- [ ] Ready to remove bridge (Week 3)

**Current Status: Week 1 Complete ✅, Ready for Week 2 🚀**

---

**Questions? Just ask:**
- "What should I do first?"
- "How do I migrate X?"
- "Can you show me an example?"
- "Let's start testing"
- "Begin migration"
