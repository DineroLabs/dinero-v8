# Phase F.5 — End-to-End Integration Test Plan

**Purpose**: Prove the system (real daemon + wallet + chainstate + mining) obeys all frozen policy contracts.

**Status**: Planning phase - test infrastructure and implementation roadmap

---

## Test Infrastructure Required

### 1. Test Harness Utilities

**TestDaemon** - Daemon lifecycle manager
```cpp
class TestDaemon {
public:
    // Start daemon with temp datadir + unique ports
    bool start(const std::vector<std::string>& args = {});

    // Graceful shutdown
    void stop();

    // Restart with same config
    bool restart();

    // Get RPC port
    int getRpcPort() const;

    // Wait for daemon ready
    bool waitForReady(int timeout_sec = 30);

private:
    std::string datadir_;
    int rpc_port_;
    int p2p_port_;
    pid_t daemon_pid_;
};
```

**RpcClient** - JSON-RPC wrapper
```cpp
class RpcClient {
public:
    RpcClient(const std::string& host, int port);

    // Generic RPC call
    Json call(const std::string& method, const Json& params = {});

    // Convenience methods
    Json mining_info();
    Json mining_start(int threads = 1, const std::string& address = "");
    Json mining_stop();
    Json mining_setaddress(const std::string& address);
    Json mining_getaddress();

    Json wallet_create(const std::string& name);
    Json wallet_encrypt(const std::string& passphrase);
    Json wallet_unlock(const std::string& passphrase, int timeout);
    Json wallet_getnewaddress(const std::string& label = "");

    Json getblockcount();
    Json generate(int nblocks);
};
```

**WalletHelper** - Wallet operations
```cpp
class WalletHelper {
public:
    WalletHelper(RpcClient& rpc);

    // Create and initialize wallet
    std::string createWallet(const std::string& name);

    // Encrypt wallet
    void encryptWallet(const std::string& passphrase);

    // Unlock wallet
    void unlockWallet(const std::string& passphrase, int timeout = 600);

    // Get mining address (owned by wallet)
    std::string getMiningAddress();

private:
    RpcClient& rpc_;
};
```

**ChainHelper** - Chain operations
```cpp
class ChainHelper {
public:
    ChainHelper(RpcClient& rpc);

    // Mine blocks
    void mineBlocks(int count);

    // Wait for specific height
    bool waitForHeight(int target_height, int timeout_sec = 30);

    // Get current height
    int getHeight();

private:
    RpcClient& rpc_;
};
```

---

## Test Groups

### F.5.1 — Happy Path Mining

**Test**: `MiningHappyPath_WalletOwnedAddress`

**Steps**:
1. Start daemon with temp datadir
2. Create wallet ("test_wallet")
3. Generate mining address
4. Call `mining.start(1, address)`
5. Wait for 1-2 blocks
6. Call `mining.info`
7. Stop mining
8. Verify wallet balance increased

**Assertions**:
- `mining.start` succeeds (no error)
- `mining.info.mining == true`
- Blocks found > 0
- Wallet balance > 0
- No policy errors in logs

**Status**: ⏳ Requires TestDaemon, RpcClient, WalletHelper, ChainHelper

---

### F.5.2 — Wallet Ownership Enforcement (E.1)

**Test**: `MiningForeignAddressRejected`

**Steps**:
1. Start daemon
2. Create wallet
3. Use foreign address (not owned by wallet)
4. Call `mining.start(1, foreign_address)`

**Assertions**:
- RPC error code == -13
- Error message contains "not owned"
- `mining.info.mining == false`
- No mining threads started

**Status**: ⏳ Requires test infrastructure

---

**Test**: `MiningWithoutWalletRejected`

**Steps**:
1. Start daemon
2. Do NOT load wallet
3. Call `mining.start(1, "din1someaddress")`

**Assertions**:
- RPC error code == -13
- Error message contains "No active wallet"
- Mining not started

**Status**: ⏳ Requires test infrastructure

---

**Test**: `MiningLockedWalletRejected`

**Steps**:
1. Start daemon
2. Create wallet
3. Encrypt wallet
4. Do NOT unlock
5. Call `mining.start(1, address)`

**Assertions**:
- RPC error code == -13
- Error message contains "locked" or "unlock"
- Mining not started

**Status**: ⏳ Requires test infrastructure

---

### F.5.3 — IBD / Reindex Safety (E.2)

**Test**: `MiningDuringIBDRejected`

**Steps**:
1. Start daemon (fresh datadir, will be in IBD state)
2. Create wallet
3. Immediately call `mining.start` (before chain sync complete)

**Assertions**:
- RPC error code == -10
- Error message contains "initial block download" or "sync"
- Mining not started

**Status**: ⏳ Requires test infrastructure
**Note**: May need to simulate IBD state or use regtest mode

---

**Test**: `MiningDuringIBDAllowedWithFlag`

**Steps**:
1. Start daemon with `--mine-during-ibd` flag
2. Create wallet
3. Call `mining.start` while in IBD state

**Assertions**:
- `mining.start` succeeds
- Mining active despite IBD

**Status**: ⏳ Requires test infrastructure

---

**Test**: `MiningDuringReindexAlwaysRejected`

**Steps**:
1. Start daemon with `-reindex` flag
2. Create wallet
3. Call `mining.start`

**Assertions**:
- RPC error code == -10
- Error message contains "reindex"
- Mining not started
- Cannot be bypassed even with `--mine-during-ibd` flag

**Status**: ⏳ Requires test infrastructure

---

### F.5.4 — Wallet Switch Protection (E.4.1)

**Test**: `WalletSwitchWhileMiningRejected`

**Steps**:
1. Start daemon
2. Create wallet
3. Start mining
4. Verify mining active
5. Attempt `mining.setaddress(foreign_address)`

**Assertions**:
- RPC error code == -13
- Error message contains "mining is active" or "mining.stop"
- Mining continues with original address
- No silent address change

**Status**: ⏳ Requires test infrastructure

---

### F.5.5 — mining.stop Semantics (E.4.2)

**Test**: `MiningStopIsIdempotent`

**Steps**:
1. Start daemon
2. Create wallet
3. Start mining
4. Call `mining.stop`
5. Verify mining stopped
6. Call `mining.stop` again
7. Call `mining.stop` third time

**Assertions**:
- First `mining.stop` succeeds
- Second `mining.stop` succeeds (no error)
- Third `mining.stop` succeeds (no error)
- All calls return success (idempotent)

**Status**: ⏳ Requires test infrastructure

---

### F.5.6 — Restart Semantics (E.3)

**Test**: `MiningDoesNotAutoResumeAfterRestart`

**Steps**:
1. Start daemon
2. Create wallet
3. Start mining
4. Verify mining active
5. Stop daemon cleanly
6. Restart daemon (same datadir)
7. Call `mining.info`

**Assertions**:
- `mining.info.mining == false` (NOT auto-resumed)
- Mining address still present in config
- Error message or hint suggests calling `mining.start` explicitly

**Status**: ⏳ Requires TestDaemon with restart support

---

**Test**: `MiningResumesOnlyAfterExplicitStart`

**Steps**:
1. After restart from previous test
2. Call `mining.start`

**Assertions**:
- `mining.start` succeeds
- Mining becomes active
- No policy errors
- Proves manual restart required

**Status**: ⏳ Requires test infrastructure

---

### F.5.7 — Escape Hatch Integration (F.4)

**Test**: `ExternalMiningAllowedWithFlag`

**Steps**:
1. Start daemon with `--allow-external-mining` flag
2. Call `mining.start(1, foreign_address)` (foreign address)

**Assertions**:
- `mining.start` succeeds
- Mining active
- Optional: Warning logged about external mining

**Status**: ⏳ Requires test infrastructure

---

**Test**: `ExternalMiningBlockedWithoutFlag`

**Steps**:
1. Start daemon normally (no `--allow-external-mining`)
2. Call `mining.start(1, foreign_address)`

**Assertions**:
- RPC error code == -13
- Error message contains "not owned"
- Mining not started

**Status**: ⏳ Requires test infrastructure

---

## Implementation Roadmap

### Phase 1: Test Infrastructure (Priority: CRITICAL)

**Tasks**:
1. ✅ Create test plan document (this file)
2. ⏳ Implement `TestDaemon` class
   - Start dinerod process
   - Manage temp datadir
   - Assign unique ports
   - Graceful shutdown
   - Restart support
3. ⏳ Implement `RpcClient` class
   - JSON-RPC over HTTP
   - Mining RPC methods
   - Wallet RPC methods
   - Chain RPC methods
4. ⏳ Implement `WalletHelper` class
   - Wallet creation
   - Encryption/unlock
   - Address generation
5. ⏳ Implement `ChainHelper` class
   - Block generation
   - Height waiting

**Estimated Effort**: Significant (infra is 50% of F.5 work)

---

### Phase 2: Critical Tests (Priority: HIGH)

**Must-have tests** (prove core contracts):
1. ⏳ `MiningHappyPath_WalletOwnedAddress` (F.5.1)
2. ⏳ `MiningForeignAddressRejected` (F.5.2 - E.1)
3. ⏳ `MiningStopIsIdempotent` (F.5.5 - E.4.2)
4. ⏳ `MiningDoesNotAutoResumeAfterRestart` (F.5.6 - E.3)

**Rationale**: These 4 tests prove:
- System works (happy path)
- E.1 enforced (wallet ownership)
- E.4.2 enforced (stop idempotency)
- E.3 enforced (restart semantics)

---

### Phase 3: Additional Coverage (Priority: MEDIUM)

**Nice-to-have tests**:
1. ⏳ `MiningWithoutWalletRejected` (F.5.2)
2. ⏳ `MiningLockedWalletRejected` (F.5.2)
3. ⏳ `WalletSwitchWhileMiningRejected` (F.5.4 - E.4.1)
4. ⏳ `ExternalMiningAllowedWithFlag` (F.5.7 - F.4)
5. ⏳ `ExternalMiningBlockedWithoutFlag` (F.5.7 - F.4)

**Rationale**: Adds completeness but not strictly necessary if Phase 2 passes

---

### Phase 4: IBD/Reindex Tests (Priority: LOW)

**Complex tests** (require chain state manipulation):
1. ⏳ `MiningDuringIBDRejected` (F.5.3 - E.2)
2. ⏳ `MiningDuringIBDAllowedWithFlag` (F.5.3 - E.2)
3. ⏳ `MiningDuringReindexAlwaysRejected` (F.5.3 - E.2)

**Rationale**: E.2 already proven by tripwire tests; integration tests are nice-to-have

---

## Success Criteria

Phase F.5 is **COMPLETE** when:

✅ Test infrastructure exists (TestDaemon, RpcClient, helpers)
✅ At least 4 critical tests pass (Phase 2)
✅ Tests run against real daemon (not mocks)
✅ All policy contracts (E.1-E.4) verified end-to-end
✅ No new policy added
✅ No production code modified

---

## Current Status

**Infrastructure**: ❌ Not implemented
**Critical Tests (Phase 2)**: ❌ Not implemented
**Additional Tests (Phase 3)**: ❌ Not implemented
**IBD/Reindex Tests (Phase 4)**: ❌ Not implemented

**Blockers**:
- Need to implement test infrastructure (TestDaemon, RpcClient)
- Need daemon binary in known location
- Need RPC communication layer

**Next Steps**:
1. Implement test infrastructure (TestDaemon, RpcClient, helpers)
2. Implement 4 critical tests (Phase 2)
3. Verify tests pass
4. Commit F.5

---

## Notes

- **Black-box testing**: Tests use RPC only, no internal service calls
- **Real daemon**: No mocks/stubs for daemon/wallet/chainstate
- **Temp datadir**: Each test uses isolated datadir
- **Port management**: Tests assign unique RPC/P2P ports
- **Fast cleanup**: Daemon stopped and temp files cleaned after each test

---

## Risk Assessment

**Low Risk**:
- Test infrastructure well-scoped
- No policy changes
- No production code changes

**Medium Risk**:
- Daemon startup/shutdown complexity
- RPC communication reliability
- Test flakiness (timing issues)

**Mitigation**:
- Use robust daemon startup detection
- Add retry logic for RPC calls
- Use generous timeouts
- Clean shutdown procedures

---

## Future Work (Post-F.5)

After F.5 complete, consider:
- Expand test coverage (Phase 3 & 4 tests)
- Performance tests (hashrate, block generation time)
- Stress tests (long-running mining, many restarts)
- Multi-wallet scenarios
- GPU mining integration tests

**Not in scope for Phase F**

---

**Document Status**: Planning complete, implementation pending
**Last Updated**: 2025-12-28
**Phase**: F.5 - End-to-End Integration Tests
