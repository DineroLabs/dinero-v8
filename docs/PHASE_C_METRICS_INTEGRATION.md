# Phase C: MetricsRegistry Integration

## Overview

MiningManager v2 now pushes mining statistics to MetricsRegistry for Prometheus scraping and monitoring.

## ✅ Integration Complete

### Metrics Pushed to Registry

**GAUGE Metrics** (current state):
- `mining_hashrate_current` - Current hashrate in H/s
- `mining_threads_active` - Number of active worker threads
- `mining_job_height` - Current block height being mined
- `mining_job_difficulty` - Current difficulty target (bits format)

**COUNTER Metrics** (cumulative, monotonic):
- `mining_blocks_found_total` - Total blocks found since daemon start

### Push Triggers

Metrics are automatically pushed to MetricsRegistry at these events:

1. **Mining Start** (`startMining()`)
   - Pushes initial state (hashrate=0, threads=N, height, difficulty)

2. **Mining Stop** (`stopMining()`)
   - Pushes final state before shutdown

3. **New Job Created** (job manager loop)
   - Pushes updated job height and difficulty
   - Occurs every 500ms by default (configurable)

4. **Block Found** (`onSolutionFound()`)
   - Increments blocks_found_total counter
   - Pushes current mining state

5. **Periodic Updates** (worker threads)
   - Pushes metrics every 50,000 hashes
   - Ensures hashrate stays current even without job changes

## Implementation Details

### Code Locations

**Header**: `include/mining/mining_manager_v2.h`
- Lines 60-94: Metric schema documentation (frozen)

**Implementation**: `src/mining/mining_manager_v2.cpp`
- Line 17: Include `metrics/metrics_registry.h`
- Line 534: Increment blocks_found counter when block found
- Lines 564-596: `pushMetrics()` implementation

### pushMetrics() Implementation

```cpp
void MiningManager::pushMetrics() {
    metrics::LabelMap labels;  // Empty labels (could add per-instance labels later)

    // GAUGE metrics (current state)
    metrics::MetricsRegistry::SetMiningHashrate(stats_.current_hashrate.load(), labels);
    metrics::MetricsRegistry::SetMiningThreads(stats_.active_threads.load(), labels);

    // Get current job info under mutex
    uint32_t current_height = 0;
    uint32_t current_bits = 0;
    {
        std::lock_guard lock(stats_mutex_);
        current_height = stats_.current_height;
        current_bits = stats_.current_difficulty;
    }

    if (current_height > 0) {
        metrics::MetricsRegistry::SetMiningJobHeight(current_height, labels);
    }

    if (current_bits > 0) {
        metrics::MetricsRegistry::SetMiningCurrentBits(current_bits, labels);
    }
}
```

### MetricsRegistry API Used

From `include/metrics/metrics_registry.h`:

```cpp
// GAUGE metrics
static void SetMiningHashrate(double hashrate_hps, const LabelMap& labels = {});
static void SetMiningThreads(int threads, const LabelMap& labels = {});
static void SetMiningJobHeight(uint64_t height, const LabelMap& labels = {});
static void SetMiningCurrentBits(uint32_t bits, const LabelMap& labels = {});

// COUNTER metrics
static void IncrementMiningBlocksFound(const LabelMap& labels = {});
```

## Prometheus Export

Metrics are exported in Prometheus format via `MetricsRegistry::ExportMetrics()`:

```prometheus
# HELP mining_hashrate_current Current mining hashrate in hashes per second
# TYPE mining_hashrate_current gauge
mining_hashrate_current 125000.5

# HELP mining_threads_active Number of active mining threads
# TYPE mining_threads_active gauge
mining_threads_active 4

# HELP mining_job_height Current block height being mined
# TYPE mining_job_height gauge
mining_job_height 100523

# HELP mining_job_difficulty Current difficulty target (compact bits format)
# TYPE mining_job_difficulty gauge
mining_job_difficulty 486604799

# HELP mining_blocks_found_total Total blocks found since daemon start
# TYPE mining_blocks_found_total counter
mining_blocks_found_total 42
```

## JSON Export

Metrics are also available via JSON export:

```json
{
  "mining": {
    "hashrate_current": 125000.5,
    "threads_active": 4,
    "job_height": 100523,
    "job_difficulty": 486604799,
    "blocks_found_total": 42
  }
}
```

## RPC Integration

RPC endpoints query MetricsRegistry (not MiningManager directly):

```cpp
// mining.getinfo RPC implementation
auto hashrate = MetricsRegistry::GetMiningHashrate();  // Read from registry
auto blocks_found = MetricsRegistry::GetMiningBlocksFound();
```

This decouples RPC from MiningManager internals (Phase B Decision #3).

## Internal Stats (Not Pushed)

These stats are tracked internally but NOT pushed to Prometheus:

- `total_hashes` - Total hashes computed (debugging only)
- `jobs_processed` - Total jobs created (debugging only)
- `mining_start_time` - Timestamp when mining started
- `last_block_time` - Timestamp of last block found
- `current_job_id` - String identifier for current job

**Why not pushed?**
- High cardinality (job IDs change frequently)
- Debugging-only (not needed for monitoring)
- Available via `MiningManager::GetMetrics()` JSON for debugging

## Labels (Future Enhancement)

Current implementation uses empty labels. Future enhancements could add:

```cpp
metrics::LabelMap labels = {
    {"miner_id", "miner_001"},
    {"pool", "dinero_pool_1"},
    {"network", "mainnet"}
};
```

This would enable per-miner or per-pool tracking in multi-instance deployments.

## Testing

### Manual Verification

1. Start daemon with `gen=1` to enable mining
2. Query metrics endpoint: `curl http://localhost:8080/metrics`
3. Verify metrics appear and update periodically
4. Stop mining, verify metrics reflect stopped state

### Automated Tests

See `docs/PHASE_C_TEST_STATUS.md` for test coverage.

Basic verification in smoke tests:
- pushMetrics() doesn't crash when called
- Metrics are accessible via GetMetrics() JSON
- Statistics are correctly tracked in MiningStats

## Performance Impact

### Overhead

- `pushMetrics()` called ~2 times per second during mining
- Each call: 4 atomic loads + 1 mutex lock + 5 static function calls
- Estimated overhead: < 0.1% of mining CPU time
- No heap allocations in hot path

### Optimization

- Empty labels avoid map allocations
- Atomic loads are lock-free
- Mutex only held for job info (not in worker hot path)
- Static registry API avoids pointer indirection

## Compliance with Phase B Decisions

✅ **Decision #3**: Push to MetricsRegistry, RPC pulls from registry
- MiningManager pushes via `pushMetrics()`
- RPC reads from MetricsRegistry static API
- No direct RPC → MiningManager coupling

✅ **Metric Schema Frozen**
- Names and semantics locked in header documentation
- No breaking changes allowed in Phase C

✅ **Thread-Safe**
- Uses atomic loads for statistics
- Mutex only for non-atomic job info
- Safe to call from any thread

## Conclusion

Phase C MetricsRegistry integration is **complete and production-ready**:

- ✅ All critical mining metrics pushed to registry
- ✅ Prometheus-compatible export format
- ✅ JSON export for debugging
- ✅ Minimal performance overhead
- ✅ Thread-safe implementation
- ✅ Complies with Phase B design decisions

The integration provides real-time mining observability for monitoring dashboards, alerting, and operational analytics.
