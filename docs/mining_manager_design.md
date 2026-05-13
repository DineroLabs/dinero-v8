# Phase B: MiningManager Redesign (Design Phase)

**Status**: Design Phase (NO CODE YET)
**Date**: 2025-12-28
**Goal**: Consolidate 4 mining implementations into 1 clean, well-designed system

---

## Executive Summary

**Problem**: DineroCoin currently has 4 different mining implementations (MiningManager, MiningService, Miner, Mining), each with different threading models, ownership patterns, and block template flows. This creates:
- Duplicate code and logic
- Unclear ownership boundaries
- Inconsistent statistics reporting
- Complex shutdown sequences
- Maintenance burden

**Solution**: Consolidate into a single, clean architecture based on the best patterns from each implementation.

---

## Current State Analysis

### Existing Implementations

| Implementation | Pattern | Thread Model | Block Template Source | Ownership |
|----------------|---------|--------------|----------------------|-----------|
| **MiningManager** | Singleton | N CPU threads + 1 GPU thread | BlockAssembler::CreateJob() | Self-owned singleton |
| **MiningService** | IService wrapper | Wraps Mining class + telemetry thread | Consensus OR Mining class | DaemonContext |
| **Miner** | Job-based | N worker threads + 1 job manager thread | BlockAssembler::CreateJob() | Dependency-injected |
| **Mining** | Legacy | N worker threads (multi-threaded mode) | Self-creates block template (500+ line method) | MiningService |

### What Works Well

1. **Miner's Job Manager Pattern**:
   - Separate thread for job management
   - Workers wait on condition variable (CPU-efficient)
   - Clean job refresh mechanism
   - Thread-safe job distribution

2. **MiningManager's GPU/CPU Separation**:
   - Disjoint nonce ranges (CPU: [0, 10M], GPU: [10M+1, MAX])
   - Separate threads prevent interference
   - Atomic solution detection

3. **MiningService's Lifecycle**:
   - Clean Init/Start/Stop pattern (IService)
   - Integrates with DaemonContext
   - Proper dependency injection

4. **BlockAssembler's Job API**:
   - `CreateJob()` returns complete MiningJob
   - `ShouldRefreshJob()` checks staleness
   - Encapsulates block template logic

### What Needs Fixing

1. **Code Duplication**: 4 implementations doing similar things
2. **Unclear Ownership**: Who starts/stops mining? RPC? Service? Manager?
3. **Thread Chaos**: 3 different threading patterns
4. **Statistics**: No single source of truth for hashrate/stats
5. **Shutdown**: Complex, order-dependent cleanup

---

## Design Decisions

### 1. Thread Model

**Decision**: Use Miner's job-based pattern with one job manager thread + N worker threads.

**Rationale**:
- **CPU-efficient**: Workers sleep on condition variable, not busy-wait
- **Scalable**: Job manager handles refresh logic, workers just hash
- **Clean separation**: Job management logic separate from hashing logic
- **GPU-ready**: Can add GPU worker threads later with disjoint nonce ranges

**Architecture**:
```
┌─────────────────────────────────────────────────────────────┐
│                      MiningManager                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────┐                                          │
│  │ Job Manager  │  ← Separate thread, owns job lifecycle  │
│  │   Thread     │     - Requests jobs from BlockAssembler  │
│  └──────┬───────┘     - Monitors staleness               │
│         │             - Broadcasts new jobs via CV         │
│         │                                                  │
│         ├─────────┐                                        │
│         ▼         ▼         ▼         ▼                    │
│    ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐          │
│    │Worker 0│ │Worker 1│ │Worker 2│ │Worker N│          │
│    │(CPU)   │ │(CPU)   │ │(CPU)   │ │(CPU)   │          │
│    └────────┘ └────────┘ └────────┘ └────────┘          │
│         │         │         │         │                    │
│         └─────────┴─────────┴─────────┘                    │
│                    │                                        │
│                    ▼                                        │
│          ┌─────────────────┐                              │
│          │ Solution Found? │                              │
│          └─────────────────┘                              │
└─────────────────────────────────────────────────────────────┘
```

**Thread Lifecycle**:
1. `startMining()` spawns job manager thread first
2. Job manager creates initial job, then spawns N worker threads
3. Workers wait on condition variable for jobs
4. Job manager periodically checks `ShouldRefreshJob()` and broadcasts new jobs
5. `stopMining()` signals shutdown, joins all threads (job manager last)

**Nonce Distribution** (prevents duplicate work):
- **CPU Workers**: Each thread i gets nonces: i, i+N, i+2N, ... (stride = thread_count)
- **GPU Workers** (future): Separate nonce range [10M+1, UINT32_MAX]

### 2. Ownership Semantics

**Decision**: MiningManager is an **IService** (not a singleton), owned by DaemonContext.

**Rationale**:
- **Clear lifecycle**: DaemonContext controls Init/Start/Stop
- **Dependency injection**: ChainDB, Mempool, BlockAssembler injected via Init()
- **Testable**: Can create multiple instances for testing
- **No globals**: Avoids singleton anti-pattern

**Ownership Chain**:
```
DaemonContext
  ├─ ChainstateService (owns ChainDB)
  ├─ MempoolService (owns Mempool)
  └─ MiningService (owns MiningManager)
        ├─ BlockAssembler (created by MiningManager)
        ├─ Job Manager Thread (owned by MiningManager)
        └─ Worker Threads (owned by MiningManager)
```

**Start/Stop Control**:
- **DaemonApp**: Calls `ctx.mining->Start()` on daemon startup if configured
- **RPC**: Can call `ctx.mining->startMining()` / `stopMining()` for manual control
- **MiningService**: Wraps MiningManager, handles lifecycle hooks

**Configuration**:
- `gen=1` in config → MiningService calls `MiningManager::startMining()` on Start()
- `gen=0` → Mining inactive until RPC command

### 3. Block Template Request Flow

**Decision**: MiningManager requests jobs from BlockAssembler via job manager thread.

**Rationale**:
- **Single source of truth**: BlockAssembler owns block template logic
- **Clean separation**: MiningManager doesn't know about block construction
- **Refresh logic centralized**: BlockAssembler::ShouldRefreshJob() decides when to refresh
- **ASERT-aware**: BlockAssembler handles timestamp and difficulty

**Request Flow**:
```
┌───────────────┐
│ Job Manager   │
│   Thread      │
└───────┬───────┘
        │
        │ (1) CreateJob()
        ▼
┌────────────────────────┐
│   BlockAssembler       │
│  ┌──────────────────┐  │
│  │ GetChainTip()    │  │ ← ChainDB
│  │ GetNextWorkReq() │  │ ← Consensus
│  │ SelectTxs()      │  │ ← Mempool
│  │ BuildCoinbase()  │  │ ← Mining address
│  │ CalcMerkleRoot() │  │
│  └──────────────────┘  │
└───────┬────────────────┘
        │
        │ (2) Returns MiningJob
        ▼
┌───────────────────────┐
│  std::shared_ptr<     │
│    MiningJob          │
│  >                    │
│  ┌─────────────────┐  │
│  │ header          │  │
│  │ transactions    │  │
│  │ target_bits     │  │
│  │ target_hex      │  │
│  │ job_id          │  │
│  │ height          │  │
│  │ block_reward    │  │
│  │ stop_mining     │  │ ← Atomic flag
│  └─────────────────┘  │
└───────┬───────────────┘
        │
        │ (3) Broadcast to workers via CV
        ▼
┌────────────────────────────────┐
│  Workers (condition variable)  │
│  ┌──────┐ ┌──────┐ ┌──────┐  │
│  │ W0   │ │ W1   │ │ W2   │  │
│  └──────┘ └──────┘ └──────┘  │
└────────────────────────────────┘
```

**Refresh Logic** (Job Manager Thread):
```cpp
while (!shutdown_requested) {
    // Check if current job is stale
    if (current_job_ && block_assembler_->ShouldRefreshJob(current_job_)) {
        // Get new job
        auto new_job = block_assembler_->CreateJob();

        // Atomically swap jobs
        {
            std::lock_guard lock(job_mutex_);
            current_job_->stop_mining = true;  // Signal workers to stop
            current_job_ = new_job;
        }

        // Wake up all workers
        job_cv_.notify_all();
    }

    std::this_thread::sleep_for(500ms);
}
```

**Staleness Detection** (BlockAssembler):
- Job age > 60 seconds
- New block received (chain tip changed)
- Mempool changed significantly (new high-fee txs)

### 4. Statistics Reporting

**Decision**: MiningManager exposes metrics via `getMiningInfo()` RPC method and optionally updates MetricsRegistry.

**Rationale**:
- **Single source of truth**: MiningManager owns all mining state
- **Pull-based**: RPC pulls stats on-demand (no background threads for stats)
- **Optional telemetry**: Can optionally push to MetricsRegistry for Prometheus
- **Thread-safe**: All stats stored in atomics

**Statistics Tracked**:
```cpp
struct MiningStats {
    std::atomic<bool> is_mining{false};
    std::atomic<uint32_t> active_threads{0};
    std::atomic<uint64_t> total_hashes{0};
    std::atomic<double> current_hashrate{0.0};
    std::atomic<uint64_t> blocks_found{0};
    std::atomic<uint64_t> jobs_processed{0};
    std::atomic<uint64_t> mining_start_time{0};
    std::atomic<uint64_t> last_block_time{0};

    // Current job info (protected by mutex, not hot path)
    std::string current_job_id;
    uint32_t current_height{0};
    uint32_t current_difficulty{0};
};
```

**Update Pattern**:
```cpp
// Workers update atomics directly (lock-free hot path)
void miningLoop() {
    while (!stop) {
        // ... hash nonce ...
        total_hashes_.fetch_add(1);

        // Periodically update hashrate (every 10k hashes)
        if (local_hashes % 10000 == 0) {
            updateHashrate();
        }
    }
}

// Hashrate calculation (not in hot path)
void updateHashrate() {
    auto now = steady_clock::now();
    auto elapsed = duration_cast<seconds>(now - last_hashrate_time_).count();
    if (elapsed >= 1) {
        uint64_t hashes = total_hashes_.exchange(0);
        double rate = static_cast<double>(hashes) / elapsed;
        current_hashrate_.store(rate);
        last_hashrate_time_ = now;
    }
}
```

**RPC Interface**:
```json
{
  "method": "mining.getinfo",
  "result": {
    "enabled": true,
    "threads": 4,
    "hashrate": 125000.5,
    "blocks_found": 3,
    "current_job": {
      "height": 1234,
      "difficulty": "0x1d31ffce",
      "job_id": "abc123"
    }
  }
}
```

**Telemetry** (optional):
- MiningService can optionally call `MiningManager::getStats()` every 5s
- Pushes to MetricsRegistry for Prometheus scraping
- NOT required for core functionality

### 5. Shutdown Semantics

**Decision**: Graceful shutdown with ordered thread joins and resource cleanup.

**Rationale**:
- **Safe**: No resource leaks or dangling threads
- **Fast**: Uses atomic flags, no polling delays
- **Predictable**: Always same shutdown order

**Shutdown Sequence**:
```
stopMining() called (from RPC or DaemonContext::Stop())
  │
  ├─ (1) Set atomic shutdown flag
  │      shutdown_requested_ = true
  │      current_job_->stop_mining = true
  │
  ├─ (2) Wake up all workers
  │      job_cv_.notify_all()
  │
  ├─ (3) Join worker threads (in any order)
  │      for (auto& thread : worker_threads_) {
  │          thread.join();
  │      }
  │
  ├─ (4) Signal job manager to stop
  │      (already sees shutdown_requested_)
  │
  ├─ (5) Join job manager thread
  │      job_manager_thread_.join()
  │
  ├─ (6) Clean up BlockAssembler
  │      block_assembler_.reset()
  │
  └─ (7) Reset state
         current_job_.reset()
         stats_.reset()
```

**Critical Invariants**:
1. Workers MUST check `shutdown_requested_` on every iteration
2. Workers MUST check `current_job_->stop_mining` before hashing
3. Job manager MUST join AFTER workers (owns the job lifecycle)
4. BlockAssembler MUST be destroyed AFTER job manager (job manager calls it)

**Fast Exit**:
- Workers check atomics every iteration (no polling delays)
- Condition variable wakeup is instant
- Typical shutdown time: < 100ms

**Edge Cases**:
- **Solution found during shutdown**: Workers check `shutdown_requested_` before claiming solution
- **Job refresh during shutdown**: Job manager exits loop immediately
- **Block submission during shutdown**: submitBlock() is safe, uses BlockAcceptor

---

## Implementation Strategy (Phase C)

**Status**: Approved - Ready to implement with user-approved design decisions.

### Step 1: Create MiningManager (IService)
- Implement IService interface (Init/Start/Stop/IsHealthy/GetMetrics)
- **Inject dependencies** (BlockAssembler&, ChainDB&, Mempool&, WalletManager&)
- Do NOT own BlockAssembler (follows approved design decision #4)
- Push stats to MetricsRegistry (follows approved design decision #3)

### Step 2: Implement Job Manager Thread
- Create job manager loop with 500ms refresh interval (approved decision #5)
- Call BlockAssembler::CreateJob() via injected reference
- Implement ShouldRefreshJob() check (chain tip, mempool, age)
- Broadcast new jobs via condition variable

### Step 3: Implement Worker Threads
- Create worker loop with condition variable (sleep when idle)
- Implement nonce striding (thread i gets nonces: i, i+N, i+2N...)
- Add solution detection logic (atomic flag)
- **CPU-only** - NO GPU in Phase C (approved decision #1)

### Step 4: Statistics Integration
- Push to MetricsRegistry: hashes computed, blocks found, uptime, errors
- RPC pulls from MetricsRegistry (NOT directly from MiningManager)
- Keep Stratum fields in MiningJob but unused (approved decision #2)

### Step 5: Wire to RPC
- Update mining RPC handlers to inject MiningManager dependencies
- RPC reads stats from MetricsRegistry (separation of concerns)
- Test with existing RPC clients

### Step 6: Remove Old Implementations
- Delete Mining class (daemon/mining.cpp) - no longer needed
- Keep Miner class for reference (may delete in Phase D)
- Update MiningService to wrap new MiningManager

### Step 7: Add Tests
- Unit tests for job management (refresh, staleness)
- Integration tests for block submission
- Stress tests for thread safety (shutdown, solution races)

---

## Design Decisions (User Approved)

### 1. GPU Mining: ❌ Defer to Phase D

**Decision**: Do NOT implement GPU in Phase C.

**Reasoning**:
- GPU mining introduces device enumeration, vendor-specific APIs (CUDA/Metal/OpenCL), async kernels, watchdog logic
- None of that is required to prove: mining correctness, reward flow, block submission
- GPU mining does not change consensus — only throughput
- **Design rule**: MiningManager must not know how hashes are computed — only that they are

**Correct Sequencing**:
- Phase C: CPU-only reference miner
- Phase D (later): GPU backend(s) as pluggable engines

### 2. Stratum Fields: ✅ Keep (but inert)

**Decision**: Keep Stratum fields in MiningJob, but don't implement Stratum protocol.

**Reasoning**:
- Stratum is a job distribution protocol, not a consensus concern
- Fields like `job_id`, `extra_nonce`, `midstate` are harmless if unused
- Data-only ≠ behavior (avoids future refactor)
- **Rules**: MiningManager does not implement Stratum now, no Stratum networking in Phase C

### 3. Statistics: ✅ Push to MetricsRegistry AND Pull via RPC

**Decision**: MiningManager pushes stats to MetricsRegistry, RPC pulls from MetricsRegistry.

**Reasoning**:
- **Separation of concerns**: MiningManager pushes (hashes computed, blocks found, uptime, errors)
- **RPC pulls from MetricsRegistry** (not directly from mining threads)
- MetricsRegistry is: thread-safe, decoupled, reusable (CLI, Prometheus, UI)
- **Anti-pattern to avoid**: RPC → MiningManager → thread state (causes races, locks, deadlocks)

### 4. BlockAssembler Ownership: ✅ Inject (don't own)

**Decision**: MiningManager does NOT own BlockAssembler — inject it.

**Reasoning**:
- BlockAssembler belongs to consensus layer (already tested and validated)
- MiningManager consumes block templates, must not mutate assembler behavior
- **Correct pattern**: `MiningManager(BlockAssembler&, ChainState&, WalletManager&)`
- **Enables**: Testability (mock assembler), reorg safety, no lifetime bugs
- **Anti-pattern**: Owning BlockAssembler risks stale templates, desync after reorg

### 5. Job Refresh Interval: ✅ 500ms

**Decision**: 500ms default is correct for Phase C.

**Reasoning**:
- Low CPU overhead, fast enough for new transactions/chain tip changes/difficulty updates
- Slower than PoW inner loop (tight), faster than block time (minutes)
- **Rule**: Job refresh ≠ nonce loop (refresh checks: chain tip changed? template invalidated? mining stopped?)
- **Future-proofing**: Make it configurable, can go lower for Stratum later

---

## Success Criteria

### Phase B (Design) - ✅ COMPLETE

- ✅ All 5 design questions answered with authoritative decisions
- ✅ Design document written and reviewed by user
- ✅ No code written (design phase only)
- ✅ User approved all design decisions

### Phase C (Implementation) - Ready to Start

Phase C is complete when:
- ⏳ MiningManager implements IService (injected dependencies)
- ⏳ Job manager thread + N worker threads working
- ⏳ BlockAssembler injected (not owned)
- ⏳ Statistics pushed to MetricsRegistry
- ⏳ RPC integration complete (pulls from MetricsRegistry)
- ⏳ Old Mining class removed
- ⏳ CPU-only (NO GPU in Phase C)
- ⏳ Tests passing (unit + integration)
- ⏳ 500ms job refresh interval (configurable)

---

## Appendix: Code Patterns (for reference, not implementation)

### IService Interface
```cpp
class MiningManager : public IService {
public:
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // Mining control
    bool startMining();
    void stopMining();

    // Configuration
    void setMiningAddress(const std::string& addr);
    void setThreadCount(int threads);

    // Statistics
    MiningStats getStats() const;
};
```

### Job Manager Loop
```cpp
void jobManagerThread() {
    while (!shutdown_requested_) {
        // Check if refresh needed
        if (!current_job_ || block_assembler_->ShouldRefreshJob(current_job_)) {
            auto new_job = block_assembler_->CreateJob();

            {
                std::lock_guard lock(job_mutex_);
                if (current_job_) {
                    current_job_->stop_mining = true;
                }
                current_job_ = new_job;
            }

            job_cv_.notify_all();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
```

### Worker Loop
```cpp
void workerThread(int thread_id) {
    while (!shutdown_requested_) {
        // Wait for job
        std::unique_lock lock(job_mutex_);
        job_cv_.wait(lock, [this] {
            return current_job_ || shutdown_requested_;
        });

        if (shutdown_requested_) break;

        auto job = current_job_;
        lock.unlock();

        // Hash with nonce striding
        for (uint32_t nonce = thread_id; !job->stop_mining; nonce += thread_count_) {
            if (tryNonce(job, nonce)) {
                onSolutionFound(job, nonce);
                break;
            }
            total_hashes_.fetch_add(1);
        }
    }
}
```

---

## Final Approved Design Summary

| Design Aspect | Decision | Key Principle |
|---------------|----------|---------------|
| **Thread Model** | Job manager + N workers | CPU-efficient, condition variable sleep |
| **Ownership** | IService (DaemonContext-owned) | No singleton, dependency injection |
| **Block Templates** | Inject BlockAssembler | Consensus layer owns templates |
| **Statistics** | Push to MetricsRegistry | RPC pulls from registry (separation) |
| **Shutdown** | Ordered joins (workers → job mgr) | Fast, predictable, no leaks |
| **GPU Support** | ❌ Defer to Phase D | CPU-only in Phase C |
| **Stratum Fields** | ✅ Keep but inert | Future-proofing, no behavior |
| **Refresh Interval** | 500ms (configurable) | Balance responsiveness vs CPU |

---

**Phase B Status**: ✅ COMPLETE - All design decisions approved by user

**Next Step**: Begin Phase C implementation with approved design

**End of Phase B Design Document**
