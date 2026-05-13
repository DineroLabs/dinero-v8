# Phase 8.5: Deterministic Runtime Remediation - Status Report

**Date**: 2026-01-12
**Status**: IN PROGRESS (Multi-Day Effort)
**Goal**: Eliminate all wall-clock time usage from Lightning (72 violations across 14 files)

## Architectural Principle (Phase 8.5)

Lightning MUST be purely event-driven and deterministic:
- ❌ NO `std::thread` (background threads forbidden)
- ❌ NO `std::chrono::system_clock` (wall time forbidden)
- ❌ NO `std::chrono::steady_clock` (monotonic time forbidden)
- ❌ NO `std::time(nullptr)` (C-style time forbidden)
- ✅ Time source: **Block height ONLY**
- ✅ Runtime model: **Purely event-driven** (no timers/polling)

## Progress Summary

### ✅ Phase 1: Infrastructure (COMPLETE)

#### 1. ITimeOracle Architecture Created
**File**: `include/lightning/time_oracle.h` (145 lines)

**Key Interfaces**:
```cpp
class ITimeOracle {
    virtual uint64_t getCurrentBlockHeight() const = 0;
    virtual uint64_t blockHeightToTimestamp(uint64_t height) const = 0;
    virtual uint64_t timestampToBlockHeight(uint64_t timestamp) const = 0;
    virtual uint64_t getCurrentTimestamp() const = 0;
};

class MockTimeOracle : public ITimeOracle { /* Testing */ };
class ProductionTimeOracle : public ITimeOracle { /* Runtime */ };
```

**Design**:
- Block height is canonical time source (deterministic)
- Unix timestamps derived from block height (for BOLT-11 compatibility)
- Formula: `timestamp = genesis_time + (height * block_interval)`
- Deterministic: Same height → same timestamp always

#### 2. Invoice Module Fixed (COMPLETE)
**Files Modified**:
- `include/lightning/invoice.h` (42 lines changed)
- `src/lightning/invoice.cpp` (35 lines changed)

**Changes**:
- Updated `Invoice::create()` - now requires `ITimeOracle*` parameter
- Updated `Invoice::is_expired()` - uses time oracle
- Updated `InvoiceBuilder` constructor - accepts time oracle
- Updated `InvoiceTracker` - stores and uses time oracle
- Updated `InvoiceManager` - propagates time oracle

**Result**: 6 violations eliminated ✅

#### 3. Logger Infrastructure Fixed (COMPLETE)
**Files Modified**:
- `src/common/logger.cpp` (10 lines changed)

**Changes**:
- Removed `#include <chrono>`
- Replaced `getCurrentTimestamp()` to return empty string
- Logs now deterministic (no wall-clock timestamps)

**Result**: Unblocked lightningd binary symbol check ✅

#### 4. LightningService Updated (PARTIAL)
**Files Modified**:
- `include/lightning/lightning_service.h` (2 lines added)

**Changes**:
- Added `#include "lightning/time_oracle.h"`
- Added member: `std::unique_ptr<ITimeOracle> m_time_oracle;`

**Status**: Ready for instantiation in InitForWallet()

### ✅ Phase 2: Enforcement Verification

#### Layer 2: Symbol Boundary Guard - **PASSING** ✅
```bash
$ ./ci/check_lightning_symbols.sh
✅ PASS: Lightning symbol boundary enforced
   Binary: lightningd
   No L1 symbols or forbidden runtime patterns detected
```

**Achievement**: lightningd (168KB) contains NO forbidden patterns

---

## 🔨 Phase 3: Component Remediation (IN PROGRESS)

### Remaining Work: 14 Files, 72 Violations

#### Priority 1: High-Violation Files (42 violations)

**1. payment_router.cpp** - 22 violations
- Payment timestamps (created_at, completed_at, failed_at)
- Route update timestamps (last_update)
- Performance measurement (steady_clock)
- MPP set ID generation (timestamp-based)

**Changes Needed**:
- Update `PaymentRouter` constructor to accept `ITimeOracle*`
- Replace all `std::chrono::system_clock::now()` → `time_oracle->getCurrentTimestamp()`
- Replace `std::chrono::steady_clock` performance measurement (remove or use event counter)
- Update payment record timestamps

**2. htlc_manager.cpp** - 13 violations
- HTLC timestamps (created, resolved, failed)
- Timeout calculations
- State transition timestamps

**Changes Needed**:
- Update `HTLCManager` constructor to accept `ITimeOracle*`
- Replace time usages with oracle calls
- Update HTLC lifecycle timestamp logic

**3. channel_manager.cpp** - 11 violations
- Channel creation/update timestamps
- State transition tracking
- Timeout calculations

**Changes Needed**:
- Update `ChannelManager` constructor to accept `ITimeOracle*`
- Replace time usages
- Update channel lifecycle timestamps

**4. lightning_service.cpp** - 8 violations
- **Critical**: Heartbeat monitoring (`m_last_heartbeat`, `m_last_block_processed`)
- Health check timestamps
- Deadlock detection

**Architectural Decision Needed**:
- Heartbeats are operational monitoring, not state machine logic
- Options:
  1. Remove heartbeat monitoring entirely (pure determinism)
  2. Move to separate monitoring subsystem (outside Lightning core)
  3. Use event counters instead of timestamps

---

#### Priority 2: Medium-Violation Files (10 violations)

**5. watchtower_server.cpp** - 5 violations
**6. lightning_peer.cpp** - 4 violations
**7. watchtower_client.cpp** - 3 violations
**8. swap_htlc.cpp** - 2 violations
**9. lightning_sweep_manager.cpp** - 2 violations
**10. gossip_manager.cpp** - 2 violations

---

#### Priority 3: Low-Violation Files (4 violations)

**11. wallet_client.cpp** - 1 violation
**12. lightning_wallet.cpp** - 1 violation
**13. lightning_events.cpp** - 1 violation
**14. channel_manager_core.cpp** - 1 violation

---

### Implementation Strategy

#### Step 1: Create Production Time Oracle
**Location**: `LightningService::InitForWallet()` (line ~135)

```cpp
// Phase 8.5: Create deterministic time oracle
// Uses block height as canonical time source
auto chain_oracle = std::make_unique<ProductionChainOracle>(*m_daemon_ctx);
m_time_oracle = std::make_unique<ProductionTimeOracle>(chain_oracle.get());
g_logger.info("⚡ Phase 8.5: Deterministic time oracle created");
```

#### Step 2: Update Component Constructors (Systematic Order)

**Order of operations** (respect dependencies):
1. **ChannelManager** (line 173) - foundational
2. **HTLCManager** (line 189) - depends on ChannelManager
3. **PaymentRouter** (line 192) - depends on both
4. **WatchtowerClient** (line 195) - monitoring
5. **LightningSweepManager** (line 205) - sweep operations

**Pattern for each component**:
```cpp
// OLD:
m_channel_mgr = std::make_unique<ChannelManager>(*m_daemon_ctx, m_db);

// NEW:
m_channel_mgr = std::make_unique<ChannelManager>(
    *m_daemon_ctx,
    m_db,
    m_time_oracle.get()  // Phase 8.5: Deterministic time
);
```

#### Step 3: Update Component Headers

For each component (e.g., `include/lightning/channel_manager.h`):

```cpp
// Add include
#include "lightning/time_oracle.h"

// Update constructor
ChannelManager(
    DaemonContext& ctx,
    std::shared_ptr<ILightningDB> db,
    ITimeOracle* time_oracle  // Phase 8.5
);

private:
    ITimeOracle* m_time_oracle;  // Phase 8.5: NOT owned
```

#### Step 4: Update Component Implementations

For each component (e.g., `src/lightning/channel_manager.cpp`):

```cpp
// Update constructor
ChannelManager::ChannelManager(
    DaemonContext& ctx,
    std::shared_ptr<ILightningDB> db,
    ITimeOracle* time_oracle
)
    : m_daemon_ctx(ctx)
    , m_db(db)
    , m_time_oracle(time_oracle)  // Phase 8.5
{
    // ...
}

// Replace all time usages
// OLD:
channel.created_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

// NEW:
channel.created_at = m_time_oracle->getCurrentTimestamp();
```

#### Step 5: Update Invoice Callers

**Locations that create invoices**:
- `LightningService::createInvoice()` - pass `m_time_oracle.get()`
- `InvoiceManager` instantiation - pass time oracle in constructor
- Test suites - create MockTimeOracle

---

## Architectural Decisions Made

### 1. Logging is Deterministic
- **Decision**: Remove all timestamps from logs
- **Rationale**: Logs must be deterministic for replay
- **Impact**: Operational visibility reduced, but determinism preserved
- **Future**: Could add event sequence numbers or block height

### 2. Time Source is Block Height
- **Decision**: Block height is canonical, timestamps derived
- **Rationale**: Blockchain provides distributed clock
- **Impact**: All time queries must go through oracle
- **Future**: Lightning gossip uses block-based expiry (already compatible)

### 3. Heartbeat Monitoring TBD
- **Decision**: PENDING - requires architectural review
- **Options**:
  1. Remove entirely (pure determinism)
  2. Move to separate monitoring subsystem
  3. Use event counters instead
- **Impact**: Affects operational monitoring and deadlock detection

---

## Testing Strategy

### Unit Tests
- **MockTimeOracle** allows full control
- Set block height, verify deterministic behavior
- Test timestamp derivation formulas

### Integration Tests
- **ProductionTimeOracle** with test chain
- Verify invoice expiry based on block height
- Test payment timeouts using CLTV

### Regression Tests
- All existing Lightning tests must pass
- Invoice creation/validation
- Payment routing
- Channel lifecycle

---

## Enforcement Status

### Layer 1: Header Boundary Guard ✅ PASSING
No forbidden L1 header includes detected

### Layer 2: Symbol Boundary Guard ✅ PASSING
lightningd binary contains no forbidden symbols

### Layer 3: Runtime Determinism Guard ❌ FAILING
72 violations remaining in Lightning library

### Layer 4: Hermetic Build Verification ⏭️ SKIPPED
Will test after Layer 3 passes

---

## Estimated Completion

### Completed: ~20% (Infrastructure + 2 modules)
- ITimeOracle architecture: ✅ Done
- invoice.h/cpp: ✅ Done
- logger.cpp: ✅ Done
- lightningd binary: ✅ Clean

### Remaining: ~80% (14 files, 72 violations)
- Component constructor updates: 14 files
- Time usage replacement: 72 sites
- Caller updates: ~50 call sites
- Test updates: ~20 test files

**Realistic Timeline**: 1-2 additional days of focused work

---

## Next Steps (Resumption Guide)

When resuming this work:

1. **Start here**: `LightningService::InitForWallet()` line 135
   - Create ProductionTimeOracle
   - Store in `m_time_oracle`

2. **Update ChannelManager**:
   - Header: Add `ITimeOracle* time_oracle` parameter
   - Implementation: Replace 11 time usages
   - InitForWallet: Pass `m_time_oracle.get()` to constructor

3. **Follow dependency order**: ChannelManager → HTLCManager → PaymentRouter

4. **Run enforcement**: `./ci/check_phase8_enforcement.sh --fast`

5. **Iterate until**: All layers pass ✅

---

## References

- **Phase 8.5 Spec**: Deterministic Restart Semantics
- **Phase 8.6 Spec**: CI Enforcement
- **time_oracle.h**: `include/lightning/time_oracle.h`
- **Enforcement Scripts**: `ci/check_phase8_enforcement.sh`

---

## Notes

- This is foundational work - every future Lightning feature will benefit
- Determinism enables replay, testing, formal verification
- Architecture is proven (invoice module demonstrates pattern works)
- Enforcement prevents regression (CI catches violations automatically)
