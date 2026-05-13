# P2 Milestone Summary

**Date**: 2025-11-02
**Status**: Planning Phase
**Previous Milestone**: P1 Complete (v0.6.2-p1-complete)

---

## Executive Summary

This document outlines the P2 (Priority 2) roadmap items that build upon the completed P1 foundation. P2 focuses on production hardening, user experience improvements, and network reliability enhancements.

### P1 Achievements Recap

All P1 items successfully completed in v0.6.1-v0.6.2:

- ✅ **DNS Seeds**: IPv4/IPv6 resolution with fallback bootstrap
- ✅ **Coinbase Maturity**: 100-block confirmation with dependency injection
- ✅ **Mempool Fee Selection**: Fee-rate based transaction prioritization
- ✅ **Telemetry**: Real metrics from all subsystems
- ✅ **WebSocket Security**: Rate limiting (token bucket) + backpressure control
- ✅ **Peer Manager Crypto**: Double-SHA256 hashing + BIP 152 block locator

**Build Status**: 100% successful across all targets (dinerod, dinero-cli, dinero-qt)
**Architecture**: Clean layer separation with RocksDB isolation

---

## P2 Roadmap Items

### 1. Wallet RPC Maturity Display

**Status**: ✅ Already Complete (v0.6.1)

**What Was Requested**:
- Add `mature` field to `listunspent` output
- Show `immature_balance` in `getbalance` response
- Display coinbase maturity in GUI

**What Was Found**:
The wallet RPC layer already implements comprehensive maturity tracking:

#### Existing Features

**`listunspent` Output** (`wallet_stage3_handlers.cpp:244-253`):
```json
{
  "txid": "...",
  "vout": 0,
  "value": "50.00000000",
  "confirmations": 85,
  "is_coinbase": true,
  "is_mature": false,
  "maturity_remaining": 15
}
```

**`getwalletinfo` Output** (`wallet_stage3_handlers.cpp:150-162`):
```json
{
  "confirmed_balance": "100.00000000",
  "immature_balance": "50.00000000",
  "total_balance": "150.00000000"
}
```

**Implementation Quality**:
- Uses `ChainHeightProvider` dependency injection (no RocksDB coupling)
- Enforces `COINBASE_MATURITY = 100` consensus rule
- Segregates balances: confirmed vs immature
- Calculates maturity countdown automatically

**Remaining Work**: GUI integration (see item #4 below)

---

### 2. WebSocket Cookie Authentication

**Status**: ⏸️ Deferred from P1

**Context**:
During P1 implementation, cookie authentication was attempted but blocked by Boost Beast API limitations. The `async_accept()` method doesn't expose HTTP headers before the WebSocket upgrade handshake completes.

**Implemented So Far**:
- Rate limiting with token bucket algorithm (100 burst, 10/sec refill)
- Backpressure control (2MB queue limit per connection)
- Message coalescing for lossy channels
- Per-connection metrics

**What's Missing**:
```cpp
// Current: async_accept() doesn't expose headers
ws_.async_accept([self](boost::system::error_code ec){
    // ❌ Can't access Authorization header here
});

// Needed: Manual HTTP upgrade parser
http::request<http::string_body> req;
http::async_read(socket_, buffer_, req, [&](error_code ec) {
    auto cookie = req[http::field::authorization];
    if (!ValidateCookie(cookie)) {
        return fail_auth();
    }
    ws_.async_accept(req, ...);  // ✅ Auth before upgrade
});
```

**Effort Estimate**: 1-2 days
**Priority**: Medium (dev mode `--dev` bypasses auth for testing)

**Implementation Steps**:
1. Create HTTP session class to intercept initial request
2. Parse `Authorization: Bearer <cookie>` header
3. Validate against `.cookie` file (RPC auth standard)
4. Only upgrade to WebSocket if valid
5. Return 401 Unauthorized on failure

**References**:
- Bitcoin Core RPC cookie auth: `src/rpcauth.cpp`
- Boost Beast WebSocket auth example: `example/websocket_server_auth.cpp`

---

### 3. Integration & Stress Testing

**Status**: 📋 Planned

#### 3.1 Rate Limiter Integration Tests

**Coverage Needed**:
- Token bucket refill accuracy (10 tokens/sec)
- Burst capacity enforcement (100 messages)
- Multi-connection isolation (no cross-talk)
- Connection cleanup (no memory leaks)
- Metrics validation (`total_allowed`, `total_rejected`)

**Test Framework**: Add to existing test suite (`tests/test_rpc.cpp`)

```cpp
TEST_CASE("WebSocket Rate Limiter") {
    RateLimiter limiter(100, 10);  // 100 burst, 10/sec

    // Allow burst
    for (int i = 0; i < 100; i++) {
        REQUIRE(limiter.AllowMessage(1));
    }

    // Throttle excess
    REQUIRE_FALSE(limiter.AllowMessage(1));

    // Refill after 1 second
    std::this_thread::sleep_for(1s);
    for (int i = 0; i < 10; i++) {
        REQUIRE(limiter.AllowMessage(1));
    }
    REQUIRE_FALSE(limiter.AllowMessage(1));
}
```

#### 3.2 P2P Sync Stress Tests

**Scenarios**:
1. **Fast Sync**: 10,000 block headers in <5 seconds
2. **Reorg Handling**: 50-block deep reorganization
3. **Block Locator Efficiency**: < 32 locators for 100,000 block chain
4. **Concurrent Peers**: 50 simultaneous connections
5. **Network Partitions**: Split-brain recovery

**Validation**:
- BIP 152 locator algorithm (first 10 linear, then exponential)
- Double-SHA256 hash correctness
- No orphan blocks (proper prev_block_hash linkage)
- No memory leaks (valgrind clean)

**Tools**:
- `test_net_simulator.cpp` (to be created)
- External testnet with multiple nodes
- Profiling: `perf`, `valgrind --leak-check=full`

**Effort Estimate**: 2-3 days

---

### 4. GUI Maturity Display

**Status**: 📋 Planned
**Priority**: High (User Experience)

**Goal**: Display immature balance and maturity countdown in dinero-qt wallet

#### Design Mockup

```
╔═══════════════════════════════════════╗
║  Wallet Balance                       ║
╠═══════════════════════════════════════╣
║  Available:      100.00000000 DIN    ║
║  Immature:        50.00000000 DIN  ⏳ ║  ← NEW
║                                       ║
║  Total:          150.00000000 DIN    ║
╚═══════════════════════════════════════╝

Recent Mining Rewards:
╔════════════════════════════════════════════════════╗
║  Block Height  │  Reward  │  Maturity              ║
╠════════════════════════════════════════════════════╣
║  1025          │  50 DIN  │  [████████░░] 85/100  ║  ← NEW
║  1024          │  50 DIN  │  [████████░░] 84/100  ║
║  950           │  50 DIN  │  ✓ Mature             ║
╚════════════════════════════════════════════════════╝
```

#### Implementation

**Files to Modify**:
- `src/qt/overviewpage.cpp` — Add immature balance row
- `src/qt/transactionrecord.cpp` — Add maturity status field
- `src/qt/transactiontablemodel.cpp` — Display "Matures in X blocks"
- `src/qt/walletmodel.cpp` — Connect to `getwalletinfo` RPC

**Code Sketch**:
```cpp
// overviewpage.cpp
void OverviewPage::updateBalance() {
    Json::Value info = rpc_client_->call("getwalletinfo");

    ui->labelBalance->setText(
        QString::fromStdString(info["confirmed_balance"].asString())
    );

    // NEW: Show immature balance
    QString immature = QString::fromStdString(info["immature_balance"].asString());
    if (immature != "0.00000000") {
        ui->labelImmature->setText(immature + " DIN ⏳");
        ui->labelImmature->setVisible(true);
    } else {
        ui->labelImmature->setVisible(false);
    }
}
```

**Visual Indicators**:
- ⏳ emoji for immature coins (or custom icon)
- Progress bar showing confirmations (0-100)
- Tooltip: "Matures in X blocks (~Y minutes)"
- Grayed-out styling for immature UTXOs

**Effort Estimate**: 1-2 days

---

### 5. Hardware Wallet Support

**Status**: ✅ Infrastructure Complete (PSBT)

**Context**:
DineroCoin already has full PSBT (Partially Signed Bitcoin Transactions, BIP 174) infrastructure from the P0 wallet safety milestone. Hardware wallet integration only requires connecting external devices to the existing PSBT layer.

**Completed Infrastructure**:
- ✅ PSBT serialization/deserialization (BIP 174 compliant)
- ✅ Transaction signing workflow
- ✅ Multisig support (P2SH, P2WSH)
- ✅ HD key derivation (BIP 32, BIP 44, BIP 84)

**Remaining Work**: Device integration

#### Supported Devices

**Ledger** (USB/HID):
- Uses Ledger SDK
- Communicates via APDU commands
- Example: Bitcoin app integration

**Trezor** (USB/Bridge):
- Uses Trezor Connect API
- WebUSB or Trezor Bridge
- Example: Trezor Model T

**Coldcard** (SD card / PSBT files):
- Air-gapped workflow
- Export PSBT → sign on Coldcard → import signed PSBT
- No USB required (highest security)

#### Implementation

**Option 1: Ledger Integration** (Most Popular)

**Files**:
- `src/wallet/ledger_interface.cpp` (new)
- External dependency: `ledger-core` or `libusb`

**Workflow**:
```cpp
class LedgerSigner {
public:
    // Enumerate connected Ledger devices
    std::vector<Device> EnumerateDevices();

    // Sign PSBT on hardware device
    PSBT SignPSBT(const PSBT& unsigned_tx, const std::string& derivation_path);

    // Get public key from device
    std::string GetPublicKey(const std::string& derivation_path);
};

// Usage
LedgerSigner ledger;
PSBT unsigned_tx = wallet->CreatePSBT(outputs);
PSBT signed_tx = ledger.SignPSBT(unsigned_tx, "m/84'/0'/0'/0/0");  // Native SegWit
std::string tx_hex = wallet->FinalizePSBT(signed_tx);
```

**Option 2: Coldcard Integration** (Air-Gapped)

**Workflow**:
1. Export unsigned PSBT to SD card
2. Insert SD into Coldcard, sign offline
3. Import signed PSBT from SD card
4. Broadcast transaction

**Files**:
- `src/wallet/psbt_file_io.cpp` (new)
- Add "Export PSBT" / "Import PSBT" to GUI

**Effort Estimate**: 3-5 days (Ledger), 1 day (Coldcard)

---

### 6. Advanced Fee Estimation

**Status**: 📋 Planned (Future)

**Current State**:
Basic fee estimator with static rates:
- High priority: 10 sat/vB
- Medium priority: 5 sat/vB
- Low priority: 1 sat/vB

**Goal**: Mempool-based dynamic fee estimation (like Bitcoin Core)

**Algorithm**:
Track historical fee rates and confirmation times to predict optimal fees.

```cpp
class FeeEstimator {
    // Track fee buckets
    struct Bucket {
        double fee_rate;  // sat/vB
        std::vector<int> confirmation_times;  // blocks
    };

    std::vector<Bucket> buckets_;

public:
    // Record actual confirmation time
    void ProcessBlock(const std::vector<Transaction>& txs, int height);

    // Estimate fee for target confirmation
    uint64_t EstimateFee(int target_blocks) {
        // Find bucket with 95% confirmation within target_blocks
        for (const auto& bucket : buckets_) {
            if (percentile(bucket.confirmation_times, 0.95) <= target_blocks) {
                return bucket.fee_rate;
            }
        }
        return min_relay_fee_;
    }
};
```

**Data Collection**:
- Track every transaction in mempool
- Record: `fee_rate`, `time_entered`, `block_confirmed`
- Persist statistics to disk (RocksDB)

**Effort Estimate**: 5-7 days
**Priority**: Medium (nice-to-have)

---

## Effort Summary

| Item | Status | Effort | Priority |
|------|--------|--------|----------|
| Wallet RPC Maturity | ✅ Complete | 0 days | High |
| WebSocket Cookie Auth | 📋 Planned | 1-2 days | Medium |
| Integration Tests | 📋 Planned | 2-3 days | High |
| GUI Maturity Display | 📋 Planned | 1-2 days | High |
| Hardware Wallet Support | 📋 Planned | 1-5 days | Medium |
| Advanced Fee Estimation | 📋 Planned | 5-7 days | Low |

**Total Effort**: 10-19 days (2-4 weeks)

---

## Recommended Sprint Plan

### Sprint 1 (Week 1): Production Hardening
1. ✅ Wallet RPC verification (0 days - already complete)
2. GUI maturity display (1-2 days)
3. WebSocket cookie authentication (1-2 days)

**Deliverable**: v0.6.3 with GUI maturity and WebSocket auth

### Sprint 2 (Week 2): Testing & Quality
1. Integration tests for rate limiter (1 day)
2. P2P sync stress tests (2-3 days)
3. Documentation updates (1 day)

**Deliverable**: v0.6.4 with comprehensive test coverage

### Sprint 3 (Week 3): Hardware Wallet
1. Coldcard PSBT file integration (1 day)
2. Ledger USB integration (3-5 days)
3. GUI hardware wallet support (1 day)

**Deliverable**: v0.7.0 with hardware wallet support

### Sprint 4 (Optional): Advanced Features
1. Dynamic fee estimation (5-7 days)
2. Fee estimation GUI integration (1 day)

**Deliverable**: v0.7.1 with smart fee estimation

---

## Success Criteria

**P2 Complete When**:

- ✅ Wallet RPC shows maturity fields (`is_mature`, `maturity_remaining`)
- ✅ Immature balance displayed separately in RPC responses
- ✅ GUI shows immature balance with visual indicators
- ✅ WebSocket enforces cookie authentication (except in `--dev` mode)
- ✅ Rate limiter passes all integration tests
- ✅ P2P sync handles 10,000+ blocks reliably
- ✅ Hardware wallet (Ledger or Coldcard) can sign transactions
- ✅ Zero memory leaks or crashes under stress testing

**Quality Metrics**:
- Build success: 100% (all targets)
- Test coverage: >80% for new code
- No critical bugs in production
- Clean valgrind run (no leaks)

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| WebSocket auth complexity | Medium | Medium | Use existing Bitcoin Core cookie logic |
| Hardware wallet USB issues | Medium | Low | Start with air-gapped Coldcard |
| GUI layout complexity | Low | Low | Reuse existing Qt components |
| Stress test failures | Medium | High | Iterative profiling + fixes |
| Fee estimation inaccuracy | High | Low | Keep static fallback rates |

---

## References

**Bitcoin Core**:
- Fee estimation: `src/policy/fees.cpp`
- PSBT hardware wallet: `src/wallet/scriptpubkeyman.cpp`
- RPC cookie auth: `src/rpcauth.cpp`

**BIPs**:
- BIP 152 (Compact Blocks): https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki
- BIP 174 (PSBT): https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki
- BIP 370 (PSBTv2): https://github.com/bitcoin/bips/blob/master/bip-0370.mediawiki

**Hardware Wallets**:
- Ledger SDK: https://github.com/LedgerHQ/ledger-app-builder
- Trezor Connect: https://github.com/trezor/connect
- Coldcard PSBT: https://coldcard.com/docs/psbt

---

## Appendix: Architecture Improvements

### Clean Abstraction Layers (Achieved in P1)

```
GUI Layer (Qt6)
  ↓ JSON-RPC
Daemon Layer (HTTP/WebSocket RPC)
  ↓ C++ API
Wallet Layer (HD keys, PSBT, balance)
  ↓ ChainHeightProvider (DI)
Consensus Layer (validation, difficulty)
  ↓ PRIVATE linkage
Storage Layer (RocksDB, isolated)
```

**Key Principles**:
1. No wallet code depends on RocksDB headers
2. Dependency injection for testability
3. CMake PRIVATE scoping for build isolation
4. Pure interfaces for layer boundaries

### CMake Hygiene

**Before (Problematic)**:
```cmake
target_link_libraries(dinero_consensus PUBLIC ${ROCKSDB_TARGET})
# ❌ RocksDB headers leak to wallet!
```

**After (Clean)**:
```cmake
target_link_libraries(dinero_consensus PRIVATE ${ROCKSDB_TARGET})
# ✅ RocksDB confined to consensus layer
```

**Verification**:
```bash
$ nm -g build/libdinero_wallet.a | grep rocksdb
# → No output (wallet is RocksDB-free) ✅
```

---

**Document Version**: 1.0
**Last Updated**: 2025-11-02
**Next Review**: After Sprint 1 completion
