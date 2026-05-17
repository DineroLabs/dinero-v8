# Week 1 Testing Checklist - Bridge Architecture Verification

## Building Status
- ✅ Architecture: Option A implemented
- ✅ Services: All 9 services bridge to globals
- ⏳ Build: Currently compiling dinerod

## Post-Build Testing Plan

### 1. Basic Startup Test
```bash
./dinero/bin/dinerod --help
./dinero/bin/dinerod --version
```

**Expected**: Clean startup, shows version info

### 2. Service Initialization Test
```bash
./dinero/bin/dinerod --regtest --datadir=/tmp/dinero_test
```

**Verify**:
- ✅ All services initialize successfully
- ✅ No crashes during Init()
- ✅ Legacy globals are set (check logs)
- ✅ Services start cleanly

**Log messages to look for**:
```
[ChainstateService] Legacy globals set (bridge pattern)
[WalletService] Legacy global g_wallet_manager → real WalletManager instance
[P2PService] Legacy global g_p2p → real P2PManager instance
[Bridge] Legacy globals set by services during initialization
```

### 3. Legacy Global Access Test

**Test that old code can access globals**:

```bash
# Start daemon in one terminal
./dinero/bin/dinerod --regtest

# In another terminal, test RPC calls that use globals
curl -X POST http://localhost:20996 -d '{"method":"getblockcount","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getnetworkinfo","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getwalletinfo","params":[]}'
```

**Expected**: 
- ✅ RPC methods work (they use `g_chain_db_direct`, `g_p2p`, `g_wallet_manager`)
- ✅ No null pointer errors
- ✅ Real data returned

### 4. Service Shutdown Test

**Test clean shutdown**:
```bash
# Start daemon
./dinero/bin/dinerod --regtest
# Press Ctrl+C

# Verify logs show:
# [ChainstateService] Chainstate shutdown complete
# [WalletService] Wallet service stopped cleanly
# [P2PService] P2P networking shutdown complete
# [Bridge] Legacy globals cleared
```

**Expected**:
- ✅ Services stop in reverse order
- ✅ Globals cleared (set to nullptr)
- ✅ No memory leaks
- ✅ Clean exit

### 5. Multi-Startup Test

**Test that services can be restarted**:
```bash
# Start daemon
./dinero/bin/dinerod --regtest
# Stop with Ctrl+C
# Start again immediately
./dinero/bin/dinerod --regtest
```

**Expected**:
- ✅ Can restart without issues
- ✅ Globals properly reset on each start
- ✅ No conflicts from previous run

### 6. RPC Method Test (Uses Globals)

**Test methods that directly use globals**:

```bash
# Methods that use g_chain_db_direct:
curl -X POST http://localhost:20996 -d '{"method":"getblockcount","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getblockchaininfo","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getblockhash","params":[0]}'

# Methods that use g_wallet_manager:
curl -X POST http://localhost:20996 -d '{"method":"getwalletinfo","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getnewaddress","params":[]}'

# Methods that use g_p2p:
curl -X POST http://localhost:20996 -d '{"method":"getnetworkinfo","params":[]}'
curl -X POST http://localhost:20996 -d '{"method":"getpeerinfo","params":[]}'
```

**Expected**: All methods return real data (not errors)

### 7. Bridge Pattern Verification

**Verify globals point to real instances**:

Add temporary debug code or check logs:
- `g_chain_db_direct` should point to ChainstateService's chain_db_
- `g_wallet_manager` should point to WalletService's wallet_mgr_
- `g_p2p` should point to P2PService's p2p_mgr_

**Expected**: All globals are non-null after Init(), nullptr after Stop()

## Success Criteria

✅ **Architecture**: Clean main.cpp with service pattern
✅ **Bridge**: All services set globals automatically
✅ **Functionality**: Old code using globals works
✅ **Lifecycle**: Clean init → start → stop
✅ **Stability**: No crashes, no memory leaks

## If Issues Found

### Problem: Linker errors
- **Check**: CMakeLists.txt includes all service files
- **Fix**: Ensure services are linked in dinero_daemon target

### Problem: Null pointer errors
- **Check**: Services set globals before calling Start()
- **Fix**: Verify Init() completes before RPC server starts

### Problem: Multiple definitions
- **Check**: Globals defined only once (in legacy_globals_stub.cpp)
- **Fix**: Use extern declarations elsewhere

### Problem: Namespace issues
- **Check**: `g_wallet_manager` is in global namespace (::g_wallet_manager)
- **Check**: `g_chain_db_direct` is in dinero namespace
- **Fix**: Match namespace usage in service files

## Next Steps After Successful Testing

1. **Week 2**: Begin gradual migration away from globals
2. **Identify**: Code paths using globals
3. **Migrate**: Update to use DaemonContext
4. **Remove**: Global bridge code as migration completes

## Notes

- Bridge pattern is TEMPORARY - eventual goal is DaemonContext only
- All services properly own their instances
- Legacy code continues to work during migration
- Clean shutdown ensures no dangling pointers

