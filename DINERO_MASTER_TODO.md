# DINERO MASTER TODO LIST

**Last Updated:** 2025-10-30
**Current Version:** 0.1.0 (commit 7c898171)

## 🚨 CRITICAL ISSUES (BLOCKERS)

### 1. WebSocket Shutdown Crash Bug ✅ FIXED
**Discovered:** 2025-10-30
**Fixed:** 2025-10-30
**Status:** RESOLVED
**Severity:** CRITICAL (was blocking all releases)

### 2. Daemon Auto-Creates Wallet on Startup ✅ FIXED
**Discovered:** 2025-10-30
**Fixed:** 2025-10-30
**Status:** RESOLVED
**Severity:** HIGH (was blocking clean testing)

**Problem:**
Daemon automatically created a wallet on startup if one didn't exist. This made clean testing impossible - every test run got a wallet created automatically, polluting the test environment.

**Impact:**
- Could not test "no wallet" scenarios
- Test environments got polluted
- Could not verify wallet creation flows
- Made testing unreliable

**Solution Implemented:**
- Removed auto-load section from daemon startup (lines 2778-2801)
- Users must now explicitly call `createhdwallet` RPC to create wallet
- WalletManager and WalletWorker still initialized (needed for blockchain scanning)
- Default behavior is now NO wallet unless explicitly requested

**Files Fixed:**
- `src/daemon/main.cpp` (lines 2775-2781 - removed auto-load, added message)

### 3. GUI Auto-Launches Daemon on Startup ✅ FIXED
**Discovered:** 2025-10-30
**Fixed:** 2025-10-30
**Status:** RESOLVED
**Severity:** MEDIUM

**Problem:**
GUI (dinero-qt) automatically launched the daemon process on startup if it wasn't already running. This prevented users from having explicit control over daemon launch and made troubleshooting difficult.

**Impact:**
- Users couldn't control daemon startup
- Difficult to troubleshoot daemon issues
- Daemon started in background without user knowledge
- No explicit control over daemon parameters

**Solution Implemented:**
- Removed auto-launch logic from `gui/src/main.cpp` (lines 311-408)
- Replaced with manual launch dialog that instructs users to start daemon themselves
- Dialog shows:
  - Instructions to manually run `./dinerod`
  - Expected daemon location
  - "Retry Connection" button to check again after manual launch
  - "Continue Without Daemon" option (wallet features disabled)
  - "Exit" option
- Users now have full control over daemon launch

**Files Fixed:**
- `gui/src/main.cpp` (lines 311-408 - replaced auto-launch with manual instructions)

---

**Problem:**
Daemon crashes on every shutdown with:
```
libc++abi: terminating due to uncaught exception of type std::__1::system_error:
mutex lock failed: Invalid argument
```

**Location:** `src/daemon/ws/ws_server.cpp:42-56` (WsSession destructor)

**Root Cause:**
- `WsSession` destructor attempts to lock `g_sessions_mutex` during shutdown
- Mutex is either already locked by cleanup code OR already destroyed (global destructor order)
- Results in crash every time daemon receives SIGTERM

**Fix Attempts:**
1. ❌ **Attempt 1 (2025-10-30):** Removed mutex lock from destructor - FAILED
   - Rationale: Avoid locking destroyed/already-locked mutex
   - Result: Second test still crashed with same error
   - Reason: Still accessing global mutex/map during shutdown

2. ✅ **Attempt 2 (2025-10-30 23:00):** Added shutdown flag with atomic<bool> - SUCCESS
   - Added `std::atomic<bool> g_shutdown_in_progress` global variable
   - `WsServer::stop()` sets flag to true before cleanup
   - `WsSession` destructor checks flag and skips cleanup if shutdown
   - Added try-catch for `std::system_error` as extra safety
   - Result: Clean shutdowns verified with multiple tests
   - No mutex access during global destruction = no crash

**Solution Details:**
```cpp
// Added to ws_server.cpp:
static std::atomic<bool> g_shutdown_in_progress{false};

// WsServer::stop() sets the flag:
void WsServer::stop() {
  g_shutdown_in_progress.store(true, std::memory_order_release);
  listener_->stop();
}

// WsSession destructor checks flag before cleanup:
~WsSession() {
  if (fd_ != -1 && !g_shutdown_in_progress.load(std::memory_order_acquire)) {
    // Safe to cleanup during normal operation
    if (g_subscriptions) {
      g_subscriptions->remove_connection(fd_);
    }
    try {
      std::lock_guard<std::mutex> lock(g_sessions_mutex);
      g_active_sessions.erase(fd_);
    } catch (const std::system_error& e) {
      // Mutex destroyed - skip cleanup
    }
  }
}
```

**Testing:**
- ✅ Multiple shutdown tests passed
- ✅ No crashes on SIGTERM
- ✅ Clean shutdown messages in log
- ✅ Binary verified: `build/dinerod` (17M, arm64, 2025-10-30 23:10)

**Files Affected:**
- `src/daemon/ws/ws_server.cpp`
- `build/dinerod` (17M, arm64, timestamped Oct 30 22:57)

**Compatibility Impact:** 🔴 HIGH - Blocks both Mac and Linux server releases

---

## 📊 BUILD COMPATIBILITY STATUS

### Current Binary Status

| Binary | Platform | Version | Timestamp | Status | WebSocket | GUI Support |
|--------|----------|---------|-----------|--------|-----------|-------------|
| `build/dinerod` | Mac arm64 | 0.1.0 | 2025-10-30 22:57 | ❌ CRASH BUG | ✅ Yes | N/A |
| `build/dinero-cli` | Mac arm64 | 0.1.0 | 2025-10-30 22:57 | ✅ Working | N/A | N/A |
| `build/dinero-miner` | Mac arm64 | 0.1.0 | 2025-10-30 22:57 | ✅ Working | N/A | N/A |
| `build/gui/dinero-qt` | Mac arm64 | 0.1.0 | 2025-10-30 22:57 | ⚠️ Untested | ✅ Yes | ✅ Yes |
| CA Server (172.93.160.131) | Linux x64 | 0.1.0 | Unknown | ⚠️ Unknown | ⚠️ Unknown | N/A |
| VA Server (173.249.195.59) | Linux x64 | 0.1.0 | Unknown | ⚠️ Unknown | ⚠️ Unknown | N/A |

### Compatibility Matrix

| Feature | Mac Daemon | Mac GUI | CA Server | VA Server | Verified? |
|---------|-----------|---------|-----------|-----------|-----------|
| WebSocket RPC | ✅ Compiled | ✅ Compiled | ❓ Unknown | ❓ Unknown | ❌ No |
| P2P Glad-Hands | ✅ Yes | N/A | ✅ Yes | ✅ Yes | ✅ Yes |
| HD Wallet | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| Wallet Encryption | ✅ Yes | ⚠️ Untested | ✅ Yes | ✅ Yes | ⚠️ Partial |
| Bech32 Addresses | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| Mining | ✅ Yes | N/A | ✅ Yes | ✅ Yes | ✅ Yes |
| Block Validation | ✅ Yes | N/A | ✅ Yes | ✅ Yes | ✅ Yes |
| ASERT DAA | ✅ Yes | N/A | ✅ Yes | ✅ Yes | ✅ Yes |

---

## 🔧 MAC-SPECIFIC FIXES NEEDING SERVER SYNC

### 1. WebSocket Shutdown Crash Fix
**Status:** ❌ NOT FIXED YET
**Mac:** Attempted fix in `src/daemon/ws/ws_server.cpp`
**Servers:** Not deployed (fix doesn't work yet)
**Action Required:** Fix the bug properly, then sync to servers

### 2. GUI WebSocket Integration
**Status:** ✅ COMPLETE ON MAC
**Mac:** Full integration in `src/gui/mainwindow.cpp` (6 subscriptions, 9 signal handlers)
**Servers:** N/A (servers run headless)
**Action Required:** None (servers don't need GUI)

### 3. Test Suite
**Status:** ✅ CREATED, ⚠️ BLOCKED BY CRASH BUG
**Mac:** `test_complete_functionality.sh` (12 tests)
**Servers:** Not deployed
**Action Required:**
- [ ] Fix shutdown crash
- [ ] Verify all 12 tests pass
- [ ] Create Linux version of test suite
- [ ] Run tests on both servers

---

## 📋 SERVER-SPECIFIC FEATURES NEEDING MAC SYNC

### 1. Production Uptime
**CA Server:** Running in production, serving blockchain
**VA Server:** Running in production, serving blockchain
**Mac:** Test builds only, not production
**Action Required:** None (Mac is development environment)

### 2. Systemd Integration
**Servers:** Running under systemd with auto-restart
**Mac:** Manual daemon execution
**Action Required:** None (Mac doesn't use systemd)

---

## 🎯 RELEASE CHECKLIST (v0.1.0)

**Status:** 🔴 BLOCKED - Cannot release until crash bug is fixed

### Pre-Release Requirements
- [ ] ❌ Fix WebSocket shutdown crash bug
- [ ] ⬜ All 12 functionality tests pass on Mac
- [ ] ⬜ Build Linux binaries with same fixes
- [ ] ⬜ Run test suite on CA server
- [ ] ⬜ Run test suite on VA server
- [ ] ⬜ Verify P2P compatibility between Mac and servers
- [ ] ⬜ Verify RPC compatibility between Mac CLI and servers
- [ ] ⬜ Test GUI with production servers (CA/VA)
- [ ] ⬜ Document all breaking changes
- [ ] ⬜ Update version number in all binaries

### Test Suite Status (Mac)
1. ✅ Binary existence and architecture
2. ✅ Daemon startup
3. ✅ WebSocket server initialization
4. ✅ RPC server initialization
5. ✅ CLI communication (getbalance)
6. ✅ Wallet functionality (getnewaddress)
7. ✅ Blockchain info queries
8. ⬜ P2P connections (blocked by crash)
9. ⬜ Miner binary (blocked by crash)
10. ⬜ GUI WebSocket support (blocked by crash)
11. ⬜ WebSocket RPC handlers (blocked by crash)
12. ⬜ Full integration test (blocked by crash)

---

## 📝 BUILD HISTORY TRACKING

### Build: 2025-10-30 22:57 (7c898171)
**Platform:** Mac arm64
**Status:** ❌ FAILED - Shutdown crash
**Changes:**
- Attempted WebSocket shutdown crash fix
- GUI WebSocket integration (6 subscriptions)
- Test suite created

**Issues:**
- Daemon crashes on every shutdown
- Only 7 of 12 tests can complete

**Next Build Requirements:**
- Must fix shutdown crash
- Must verify fix with multiple test runs
- Must pass all 12 tests before proceeding

---

## 🚀 DEPLOYMENT PIPELINE

### Phase 1: Development (Current)
- [x] Mac development environment
- [x] GUI WebSocket integration
- [x] Test suite creation
- [ ] Fix all critical bugs
- [ ] Pass all tests

### Phase 2: Server Deployment
- [ ] Build Linux binaries with verified fixes
- [ ] Deploy to CA server (172.93.160.131)
- [ ] Deploy to VA server (173.249.195.59)
- [ ] Run test suite on both servers
- [ ] Verify P2P between Mac and servers

### Phase 3: Release
- [ ] Create release packages (Mac + Linux)
- [ ] Generate checksums
- [ ] Create release notes
- [ ] Tag version in git
- [ ] Distribute to users

---

## 🔍 COMPATIBILITY RULES

### P2P Protocol
**Version:** Glad-Hands Protocol
**Status:** ✅ Stable
**Compatibility:** Mac ↔ CA Server ↔ VA Server all compatible

### RPC Protocol
**Version:** JSON-RPC 2.0
**Status:** ✅ Stable
**Compatibility:** Mac CLI works with Mac daemon and both servers

### WebSocket Protocol
**Version:** Phase 2.2
**Status:** ⚠️ In Development
**Compatibility:** Unknown - needs testing between Mac GUI and servers

### Block Validation
**Version:** ASERT DAA
**Status:** ✅ Stable
**Compatibility:** All platforms using same rules

---

## 📚 DOCUMENTATION NEEDED

- [ ] `DINERO_BUILD_RULES.md` - Build compatibility requirements
- [ ] `WEBSOCKET_PROTOCOL.md` - WebSocket RPC specification
- [ ] `RELEASE_PROCESS.md` - Step-by-step release checklist
- [ ] `TESTING_GUIDE.md` - How to run test suite
- [ ] `TROUBLESHOOTING.md` - Common issues and fixes

---

## 🐛 KNOWN ISSUES

### Critical (Blocking)
**NONE** - All critical issues resolved! ✅

### High Priority
1. ⚠️ GUI not rebuilt with manual daemon launch dialog
2. ⚠️ Test suite incomplete (needs verification run)
3. ⚠️ Linux binaries not built with latest fixes (WebSocket, wallet, GUI)

### Medium Priority
1. ⚠️ Database file permissions warnings (not critical)
2. ⚠️ Test address regex was too strict (fixed)

### Low Priority
1. ℹ️ No systemd integration for Mac (not needed)

---

## 📞 CONTACTS & RESOURCES

**Production Servers:**
- CA Server: `root@172.93.160.131` (SSH key: `~/.ssh/dinero_deployment_2025`)
- VA Server: `root@173.249.195.59` (SSH key: `~/.ssh/dinero_deployment_2025`)

**Key Files:**
- Test Suite: `/Users/haydarevich/Documents/DineroCoin/test_complete_functionality.sh`
- Build Directory: `/Users/haydarevich/Documents/DineroCoin/build/`
- GUI Source: `/Users/haydarevich/Documents/DineroCoin/src/gui/`
- WebSocket: `/Users/haydarevich/Documents/DineroCoin/src/daemon/ws/`

---

## 🔄 MAC FIXES PENDING LINUX DEPLOYMENT

**CRITICAL**: The following fixes were implemented on Mac but Linux servers still have OLD CODE!

### 1. ❌ **DAA Mismatch Bug Fix (CONSENSUS-CRITICAL)**
**Status:** ✅ FIXED ON MAC | ❌ NOT DEPLOYED TO LINUX
**Severity:** 🔴 CRITICAL - Causes chain to stop at height 10+ with `bad-diffbits` error

**Problem:**
- Miner produces blocks with ASERT difficulty (0x1d3fffff)
- Validator expects Bitcoin-style EDA difficulty (0x1d7ffffe)
- Results in rejected blocks: `bad-diffbits: block has X, required Y`
- Emergency Difficulty Adjustment triggers with bogus timestamps

**Mac Files Fixed:**
- `src/consensus/consensus.hpp` (line 20): Changed `easyPhaseEnd = 1` → `easyPhaseEnd = 0`
- `src/consensus/pow.hpp` (lines 3-5): Enabled ASERT, disabled Bitcoin DAA
- `src/consensus/pow.hpp` (lines 88-110): Replaced Bitcoin-style call with ASERT-only

**Linux Status:** ❌ **SERVERS STILL RUNNING OLD CODE WITH BITCOIN DAA**
- CA Server (172.93.160.131): May have DAA mismatch
- VA Server (173.249.195.59): May have DAA mismatch

**Deployment Required:**
1. Verify Linux servers have same DAA mismatch
2. Apply consensus.hpp fix (easyPhaseEnd = 0)
3. Apply pow.hpp fix (ASERT-only, no Bitcoin DAA)
4. Rebuild both CA and VA binaries
5. Test mining past height 10 on both servers
6. Verify ASERT logs appear (not Bitcoin-style DAA logs)

### 2. ✅ **WebSocket Shutdown Crash Fix**
**Status:** ✅ FIXED ON MAC | ❓ UNKNOWN ON LINUX
**Severity:** 🔴 HIGH - Daemon crashes on every shutdown

**Mac Files Fixed:**
- `src/daemon/ws/ws_server.cpp`: Added atomic shutdown flag

**Linux Status:** ❓ Need to check if Linux servers crash on shutdown

### 3. ✅ **Wallet Auto-Creation Fix**
**Status:** ✅ FIXED ON MAC | ❓ UNKNOWN ON LINUX
**Severity:** 🟡 MEDIUM - Makes testing difficult

**Mac Files Fixed:**
- `src/daemon/main.cpp` (lines 2775-2781): Removed auto-load

**Linux Status:** ❓ Need to check if Linux servers auto-create wallets

### 4. ✅ **GUI Manual Daemon Launch**
**Status:** ✅ FIXED ON MAC | N/A (GUI-only fix)
**Severity:** 🟢 LOW - GUI enhancement only

**Mac Files Fixed:**
- `gui/src/main.cpp` (lines 311-408): Manual launch dialog

**Linux Status:** N/A - Servers run headless (no GUI)

### 5. ✅ **GUI Peers & Template Tabs**
**Status:** ✅ ADDED ON MAC | N/A (GUI-only feature)
**Severity:** 🟢 LOW - GUI enhancement only

**Mac Files Fixed:**
- `gui/src/mainwindow.cpp` (lines 766-870): Added Peers and Template tabs
- `gui/src/mainwindow.cpp` (lines 1313-1324): Wired up RPC handlers

**Linux Status:** N/A - Servers run headless (no GUI)

---

## 🎯 NEXT IMMEDIATE ACTIONS

1. ✅ ~~**FIX CRASH BUG**~~ - COMPLETED (atomic shutdown flag)
2. ✅ ~~**Fix wallet auto-creation**~~ - COMPLETED (removed auto-load)
3. ✅ ~~**Fix GUI auto-launch**~~ - COMPLETED (manual daemon launch)
4. ✅ ~~**Rebuild GUI binary**~~ - COMPLETED (Peers & Template tabs added)
5. ✅ ~~**Add Peers & Template tabs**~~ - COMPLETED (2025-10-30 23:55)
6. **CRITICAL: Verify Linux DAA bug** - Check if servers have Bitcoin DAA mismatch
7. **CRITICAL: Deploy DAA fix to Linux** - Apply consensus fix to CA + VA servers
8. **Test ASERT on Linux** - Mine blocks past height 10, verify no bad-diffbits
9. **Deploy other Mac fixes to Linux** - WebSocket crash, wallet auto-creation
10. **Full integration test** - Mac ↔ Linux server compatibility
11. **Release** - Only after all platforms pass tests

---

**⚠️ RULE #1: NO MORE "ROULETTE BUILDS"**

Every build must:
1. Have a documented purpose
2. Pass the full test suite
3. Be tracked in this file
4. Have compatibility verified
5. Be tested before release

**NO EXCEPTIONS.**
