# Phase N.4 Cleanup Summary

## Date: 2025-12-21

### Problem Identified

**Linker Errors:**
```
Undefined symbols for architecture arm64:
  "dinero::network::NetworkInvariants::checkAll()"
  "dinero::network::NetworkInvariants::NetworkInvariants(...)"
  "_g_network_manager"
```

### Root Cause Analysis

1. **NetworkInvariants Missing from Build**
   - `src/network/network_invariants.cpp` existed but wasn't in CMakeLists.txt
   - Fixed by adding to build targets

2. **Stale Compiled Object Files**
   - `rpc_context_checknetworkinvariants` function removed from source
   - But old `.o` files still referenced it
   - Function violated its own file's design pattern (used `extern g_network_manager` instead of DaemonContext)

3. **g_network_manager Global Variable**
   - Temporarily added to fix linker errors
   - Removed after clean rebuild
   - Not needed (function using it was dead code)

### Actions Taken

1. ✅ Added `src/network/network_invariants.cpp` to CMakeLists.txt (both dinerod targets)
2. ✅ Removed temporary `g_network_manager` definition from `src/daemon/ws_globals.cpp`
3. ✅ Ran `make clean` to remove stale object files
4. ✅ Rebuilt `dinerod` - **SUCCESS (no errors)**
5. ✅ Rebuilt and ran `test_block_download_scheduler` - **ALL TESTS PASS**

### Architecture Findings

**Two P2P Systems Coexist:**

1. **NetworkManager** (legacy, test-only)
   - Location: `src/daemon/network_manager.cpp`
   - NOT in DaemonContext
   - Phase N.3 + N.4 integrated here
   - Used only in unit tests
   - `initializeHeaderSyncP2P()` and `initializeBlockDownloadScheduler()` never called in production

2. **P2PService** (production, context-aware)
   - Location: `src/daemon/p2p_manager.cpp` (wrapped by `P2PService`)
   - IS in DaemonContext (line 135)
   - Properly dependency-injected
   - Used by production daemon
   - Does NOT have Phase N integration yet

### Phase N.4 Status

✅ **Phase N.4 Complete and Working**
- BlockDownloadScheduler implemented
- Unit tests passing
- P2P callbacks wired (getdata, block receive)
- Block validation working
- Single in-flight constraint enforced
- No chainstate activation (as required)

⚠️ **Architecture Issue: Phase N is Test-Only**
- HeaderSyncP2P and BlockDownloadScheduler work in unit tests with NetworkManager
- Production daemon uses P2PService which doesn't have Phase N integration
- Phase N components need to be migrated to work with P2PService

### Future Work (Phase N.5+)

**Option A: Migrate Phase N to P2PService** (Recommended)
1. Extract HeaderSyncP2P and BlockDownloadScheduler as standalone components
2. Add them to DaemonContext
3. Wire to P2PService callbacks (like ChainstateService is)
4. Delete NetworkManager
5. Update unit tests to use P2PService

**Why Option A:**
- Production daemon already uses P2PService
- Maintains clean dependency injection pattern
- Avoids global variables
- Preserves all Phase N work

### Files Modified

- `CMakeLists.txt` - Added network_invariants.cpp to build
- `src/daemon/ws_globals.cpp` - Removed temporary g_network_manager global
- No other changes (source was already clean)

### Verification

```bash
# Build succeeded
make dinerod
# Output: [100%] Built target dinerod

# Tests pass
./bin/test_block_download_scheduler
# Output: === ALL BLOCK DOWNLOAD TESTS PASSED ===
```

### Conclusion

**Phase N.4 is production-ready from a testing perspective**, but requires architectural migration to integrate with the production P2PService system. The cleanup successfully resolved all linker errors and confirmed the codebase is in a clean state.
