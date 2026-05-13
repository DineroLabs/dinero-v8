# Phase 4 Roadmap: Production Deployment & Enhancement

**Status**: Planning
**Prerequisites**: Phase 3 Complete ✅
**Target Start**: Immediate (after Phase 3 verification)
**Estimated Duration**: 4-6 weeks

---

## Overview

Phase 4 focuses on production deployment, real-world monitoring, and incremental enhancements to the Architecture V3 foundation. This phase is designed to validate the Phase 3 work under production load and add production-critical features.

---

## Phase 4A: Production Rollout 🚀

**Goal**: Deploy Architecture V3 to production and validate performance
**Timeline**: Week 1-2
**Status**: Ready to begin

### Pre-Deployment Checklist

#### Build Verification
```bash
# Release build
cmake -DCMAKE_BUILD_TYPE=Release -B build-release
cmake --build build-release -j$(nproc)

# Verify no banned globals
./scripts/check_globals.sh

# Run full test suite
cd build-release && ctest --output-on-failure

# Architecture regression tests
./build-release/bin/test_architecture_regression
```

**Expected**: ✅ All tests pass, zero banned globals

#### Packaging
```bash
# Create release package
./build-standalone-macos.sh  # or build-linux-complete.sh

# Verify binaries
./DineroCoin-vNext-ArchitectureV3/bin/dinerod --version
./DineroCoin-vNext-ArchitectureV3/bin/dinero-cli --version

# Test basic functionality
./DineroCoin-vNext-ArchitectureV3/bin/dinerod --regtest &
./DineroCoin-vNext-ArchitectureV3/bin/dinero-cli blockchain.getblockcount
```

**Expected**: Clean startup, wallet notifications wired

### Staging Deployment

#### Infrastructure Setup
1. **Staging Node**: Deploy one full node with Architecture V3
2. **Mirror Node**: Deploy second node for redundancy
3. **Monitoring**: Set up basic logging and health checks

#### Configuration
```bash
# staging.conf
network=staging
datadir=/var/lib/dinero/staging
rpcport=20998
p2pport=20999
gen=0  # Manual mining control

# Enable detailed logging
debug=wallet
debug=rpc
debug=mempool
debug=net
```

#### Deployment Steps
```bash
# 1. Stop existing daemon
systemctl stop dinerod

# 2. Backup existing data
tar -czf dinero-backup-$(date +%Y%m%d).tar.gz /var/lib/dinero

# 3. Deploy new binary
cp ./build-release/bin/dinerod /usr/local/bin/dinerod-v3
ln -sf /usr/local/bin/dinerod-v3 /usr/local/bin/dinerod

# 4. Start with staging config
systemctl start dinerod

# 5. Verify startup
journalctl -u dinerod -f | grep "Wallet event notifications wired"
journalctl -u dinerod -f | grep "Registered wallet notifier"
```

**Success Criteria**:
- ✅ Daemon starts without errors
- ✅ Wallet notifications wired message appears
- ✅ RPC server responsive
- ✅ P2P connections established

### Observation Period (24-48 hours)

#### Key Metrics to Monitor

| Metric | Target | Measurement |
|--------|--------|-------------|
| **Block event → wallet update latency** | ≤ 10ms | Log timestamps in BlockAcceptor |
| **Chainstate commit → DB flush** | < 50ms | RocksDB write batch timing |
| **Memory footprint (DaemonContext)** | < 512 bytes | Memory profiler |
| **RPC throughput** | ≥ Phase 2 baseline | RPC request/sec counter |
| **Wallet sync accuracy** | 100% | Compare wallet balance vs blockchain |
| **Block processing rate** | ≥ 10 blocks/sec | Block validation throughput |

#### Monitoring Commands
```bash
# Watch wallet notifications in real-time
tail -f /var/lib/dinero/staging/debug.log | grep "notifyBlockConnected"

# Check memory usage
ps aux | grep dinerod | awk '{print $6}'  # RSS in KB

# Monitor RPC performance
while true; do
  time ./dinero-cli blockchain.getblockcount
  sleep 1
done

# Check notification latency
grep "Wallet notifications dispatched" /var/lib/dinero/staging/debug.log | \
  tail -100 | \
  awk '{print $NF}' | \
  sort -n
```

#### Red Flags (Rollback Triggers)
- ❌ Missed wallet events (balance mismatch)
- ❌ Memory leak (>10% growth over 24h)
- ❌ Block processing >500ms consistently
- ❌ RPC timeouts or failures
- ❌ Wallet database corruption
- ❌ Reorg handling failures

### Promotion Gate

**Requirements for Production Promotion**:
1. ✅ **Zero missed wallet events** - 100% notification delivery
2. ✅ **Zero rollback inconsistencies** - All reorgs handled cleanly
3. ✅ **Zero reorg sync issues** - Wallet state consistent after reorg
4. ✅ **Memory stable** - No leaks over 48h period
5. ✅ **Performance ≥ baseline** - Block processing and RPC equivalent or better
6. ✅ **No crashes** - 48h uptime without restart

**Approval Process**:
1. Review metrics log (automated report)
2. Manual verification of wallet balances
3. Stress test with 1000 blocks
4. Sign-off from lead developer
5. Tag release: `vNext-ArchitectureV3`

### Production Deployment

```bash
# 1. Schedule maintenance window (30 min)
# 2. Announce on Discord/Twitter
# 3. Deploy to production nodes (rolling update)

# Node 1
systemctl stop dinerod
cp /path/to/dinerod-v3 /usr/local/bin/dinerod
systemctl start dinerod
# Wait 10 min, verify health

# Node 2
systemctl stop dinerod
cp /path/to/dinerod-v3 /usr/local/bin/dinerod
systemctl start dinerod
# Verify both nodes syncing

# 4. Monitor for 2 hours
# 5. Announce successful upgrade
```

---

## Phase 4B: Reorg Handling 🔄

**Goal**: Add deep reorg resilience with wallet rollback
**Timeline**: Week 3-4
**Dependencies**: Phase 4A complete + field data

### Current State
- ✅ `onBlockConnected()` implemented and tested
- ⏳ `onBlockDisconnected()` - Interface exists, not implemented
- ⏳ Wallet UTXO rollback logic - Not implemented

### Implementation Plan

#### 1. Implement WalletManager::onBlockDisconnected()

**File**: `src/wallet/wallet_manager.cpp`

```cpp
void WalletManager::onBlockDisconnected(const Block& block, uint32_t height) {
    std::lock_guard<std::mutex> lock(wallet_mutex_);

    if (!active_wallet_) {
        return;  // No wallet to update
    }

    LogPrintf("[WalletManager] Processing block disconnect at height %d\n", height);

    // 1. Mark created UTXOs as spent (rollback)
    for (const auto& tx : block.vtx) {
        std::string txid = tx.GetHash();

        // Remove UTXOs created by this transaction
        for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
            if (isAddressMine(tx.vout[vout].scriptPubKey)) {
                removeUTXO(txid, vout);
                LogPrintf("[WalletManager] Removed UTXO %s:%d (rollback)\n",
                         txid, vout);
            }
        }
    }

    // 2. Restore spent UTXOs (un-spend)
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;  // Coinbase has no inputs to restore

        for (const auto& input : tx.vin) {
            // Check if we spent this input
            if (wasSpentInBlock(input.prevout.hash, input.prevout.n, height)) {
                restoreUTXO(input.prevout.hash, input.prevout.n);
                LogPrintf("[WalletManager] Restored UTXO %s:%d (un-spent)\n",
                         input.prevout.hash, input.prevout.n);
            }
        }
    }

    // 3. Update wallet tip
    setWalletTip(height - 1);

    LogPrintf("[WalletManager] ✅ Block %d disconnected, wallet rolled back\n", height);
}
```

#### 2. Add UTXO Spent Tracking

**New Schema** (`database/schema/wallet_schema.sql`):
```sql
CREATE TABLE IF NOT EXISTS utxo_spent_history (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    spent_in_block INTEGER NOT NULL,  -- Height where UTXO was spent
    spent_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (txid, vout, spent_in_block)
);

CREATE INDEX idx_spent_block ON utxo_spent_history(spent_in_block);
```

#### 3. Test Reorg Scenarios

**Test Cases**:
```cpp
TEST(WalletReorg, SimpleReorg) {
    // 1. Mine chain A: 0 → 1 → 2 → 3
    // 2. Verify wallet has UTXOs from blocks 1,2,3
    // 3. Disconnect block 3, 2
    // 4. Mine chain B: 1 → 2' → 3' → 4'
    // 5. Verify wallet state matches chain B
    EXPECT_EQ(wallet.getBalance(), expected_balance_chain_b);
}

TEST(WalletReorg, DeepReorg) {
    // Test 100-block reorg
    // Verify wallet correctly handles large rollback
}

TEST(WalletReorg, DoubleSp end) {
    // Spend UTXO in chain A
    // Reorg to chain B where same UTXO spent differently
    // Verify wallet handles conflict correctly
}
```

### Success Criteria
- ✅ Wallet balance correct after 10-block reorg
- ✅ Wallet balance correct after 100-block reorg
- ✅ No orphaned UTXOs in wallet database
- ✅ Spent history accurate
- ✅ Performance: <100ms for 100-block rollback

---

## Phase 4C: Metrics & Telemetry 📊

**Goal**: Add Prometheus metrics for monitoring
**Timeline**: Week 5-6 (optional)
**Dependencies**: Phase 4A deployed

### Metrics to Expose

#### Wallet Notification Metrics
```cpp
// Prometheus counter
dinero_wallet_notifications_total{status="success"}
dinero_wallet_notifications_total{status="error"}

// Prometheus histogram (latency in ms)
dinero_wallet_notification_latency_ms{quantile="0.5"}  // Median
dinero_wallet_notification_latency_ms{quantile="0.95"}  // 95th percentile
dinero_wallet_notification_latency_ms{quantile="0.99"}  // 99th percentile

// Prometheus gauge
dinero_registered_wallets
```

#### Block Processing Metrics
```cpp
dinero_block_processing_duration_ms{quantile="0.5"}
dinero_block_connect_success_total
dinero_block_connect_failure_total
```

#### RPC Metrics
```cpp
dinero_rpc_requests_total{method="blockchain.getblockcount"}
dinero_rpc_duration_ms{method="blockchain.getblockcount", quantile="0.95"}
```

### Implementation

**File**: `include/metrics/wallet_metrics.h`
```cpp
namespace dinero {
namespace metrics {

class WalletMetrics {
public:
    static void recordNotification(bool success, double latency_ms);
    static void recordWalletRegistration(int wallet_count);

private:
    static Counter notification_counter_;
    static Histogram notification_latency_;
    static Gauge registered_wallets_;
};

}} // namespace dinero::metrics
```

### Grafana Dashboard

**Panels**:
1. Wallet notification rate (requests/sec)
2. Notification latency (P50, P95, P99)
3. Block processing throughput
4. RPC request rate by method
5. Memory usage trend
6. Active wallet count

---

## Phase 4D: CI/CD Hardening 🔧

**Goal**: Enforce architecture standards in CI
**Timeline**: Week 5 (parallel with 4C)

### GitHub Actions Workflow

**File**: `.github/workflows/architecture-tests.yml`
```yaml
name: Architecture Regression Tests

on: [push, pull_request]

jobs:
  architecture-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Build
        run: |
          cmake -B build
          cmake --build build -j$(nproc)

      - name: Run Architecture Tests
        run: |
          cd build
          ./bin/test_architecture_regression

      - name: Check for Banned Globals
        run: |
          ./scripts/check_globals.sh
          if [ $? -ne 0 ]; then
            echo "❌ Banned globals detected!"
            exit 1
          fi

      - name: Verify Notification System
        run: |
          # Verify WalletNotifier interface exists
          grep -r "class WalletNotifier" include/

          # Verify registration in DaemonApp
          grep -r "registerWalletNotifier" src/daemon/daemon_app.cpp
```

### Required Checks
- ✅ Architecture regression tests pass
- ✅ No banned globals detected
- ✅ Build succeeds with -Werror
- ✅ All unit tests pass
- ✅ Code coverage ≥ 80%

---

## Phase 4E: Developer Experience 📚

**Goal**: Update documentation for contributors
**Timeline**: Ongoing

### README Update

Add section:
```markdown
## Architecture

Dinero v3.0 uses a modern, dependency-injected architecture:

- **DaemonContext**: Service registry (no global variables)
- **Event-Driven**: Wallet notifications via observer pattern
- **Context-Aware RPC**: Clean service access

See [docs/ARCHITECTURE_V3.md](docs/ARCHITECTURE_V3.md) for details.
```

### Contributor Guide

**File**: `docs/CONTRIBUTING.md`
```markdown
## Architecture Guidelines

### Do NOT Use Global Variables
❌ Bad:
```cpp
extern Mempool* g_mempool;
g_mempool->addTransaction(tx);
```

✅ Good:
```cpp
void handler(ExecutionContext& ctx) {
    ctx.mempool_service->addTransaction(tx);
}
```

### Service Injection
Always inject services via constructor or ExecutionContext.

### Event Notifications
To receive blockchain events, implement `WalletNotifier` interface.
```

---

## Success Metrics (Phase 4 Overall)

| Metric | Target | Status |
|--------|--------|--------|
| **Production Uptime** | >99.9% | TBD |
| **Wallet Sync Accuracy** | 100% | TBD |
| **Block Processing** | <10ms P95 | TBD |
| **Memory Leak** | 0 | TBD |
| **Reorg Handling** | 100-block deep | TBD |
| **Test Coverage** | >80% | TBD |
| **Documentation** | Complete | ✅ |

---

## Risk Mitigation

### Rollback Plan
1. Keep Phase 2 binaries available
2. Document rollback procedure
3. Practice rollback in staging
4. Maintain backup of all databases

### Monitoring Alerts
- Block processing >100ms for 5 min → Page on-call
- Memory growth >20% → Warning
- Wallet balance mismatch → Critical alert
- RPC timeout rate >1% → Warning

---

## Timeline Overview

```
Week 1-2: Phase 4A (Production Rollout)
  ├─ Build & package
  ├─ Deploy to staging
  ├─ 48h observation
  └─ Promote to production

Week 3-4: Phase 4B (Reorg Handling)
  ├─ Implement onBlockDisconnected()
  ├─ Add UTXO spent tracking
  ├─ Test deep reorgs
  └─ Deploy to production

Week 5-6: Phase 4C & 4D (Optional)
  ├─ Add Prometheus metrics
  ├─ CI/CD integration
  └─ Grafana dashboards

Ongoing: Phase 4E (Developer Experience)
  └─ Documentation updates
```

---

## Conclusion

Phase 4 validates the Architecture V3 work under production load and adds critical production features. The phased approach ensures stability while enabling continuous improvement.

**Key Principle**: Measure, monitor, iterate.

Every phase gate requires real-world data before proceeding. This data-driven approach ensures production stability while maintaining development velocity.

**Status**: Ready to begin Phase 4A deployment 🚀

---

**Document Version**: 1.0
**Last Updated**: November 11, 2025
**Next Review**: After Phase 4A completion
