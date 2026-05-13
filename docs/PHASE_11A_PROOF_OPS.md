# Phase 11a: Utreexo Proof Operations & Hardening

**Status**: Planning
**Dependencies**: Phase 10e (Utreexo mining complete)
**Date**: 2026-01-17

---

## One-Sentence Summary

**Phase 11a hardens existing Utreexo proof infrastructure for production use: batch RPCs, proof caching, stateless mode improvements, and comprehensive monitoring.**

---

## Context: What Already Exists

✅ **Already Implemented** (Phase 7.4.3, Phase 8, Phase 9):
- Stateless node mode (`--utreexo-stateless` flag)
- Proof generation (`UtreexoProofGenerator`)
- Proof relay/distribution (`UtreexoProofRelay`)
- Fast sync service (`FastSyncService`)
- Utreexo RPCs:
  - `getutreexoroots` - Get current forest roots
  - `getutreexocommitment` - Get single 32-byte commitment
  - `getutxoproof <txid> <vout>` - Generate proof for a UTXO
  - `getutreexostats` - Get accumulator statistics
  - `rebuildutreexo` - Rebuild accumulator from UTXO set

**Files**:
- `src/rpc/methods_utreexo.cpp` - Existing RPCs
- `src/consensus/utreexo_proof_generator.cpp` - Proof generation
- `src/consensus/utreexo_proof_relay.cpp` - Proof distribution
- `src/network/stateless_node.cpp` - Stateless node implementation
- `src/daemon/services/fast_sync_service.cpp` - Fast sync
- `tests/consensus/test_utreexo_stateless_validation.cpp` - Tests

---

## Phase 11a Scope: What's Missing

### 1. Batch Proof RPCs (Performance)
**Problem**: Current `getutxoproof` handles one UTXO at a time. Block explorers, wallets, and Lightning nodes need to fetch proofs for multiple UTXOs efficiently.

**Solution**: Add batch RPCs:
- `getutxoproofs_batch` - Generate proofs for multiple UTXOs in one call
- `verifyutxoproofs_batch` - Verify multiple proofs against accumulator
- Performance target: 1000+ proofs/second

### 2. Proof Cache Service (Latency)
**Problem**: Regenerating proofs on every query is expensive (O(log n) tree traversals). No LRU cache for recently requested proofs.

**Solution**: Implement in-memory proof cache:
- LRU cache with configurable size (default: 10,000 proofs)
- TTL-based expiration (invalidate on new blocks)
- Cache hit/miss metrics
- Separate from mempool/UTXO cache

### 3. Stateless Mode Hardening (Reliability)
**Problem**: Stateless mode exists but lacks:
- Graceful degradation when proofs unavailable
- Peer selection for proof sources
- Proof request retries with backoff
- Monitoring/metrics for stateless sync

**Solution**: Harden stateless node:
- Multi-peer proof fetching with fallback
- Exponential backoff for failed proof requests
- Stateless-specific logging/metrics
- Health checks for proof availability

### 4. Fast Sync Optimizations (Sync Speed)
**Problem**: Fast sync works but could be faster:
- Sequential proof fetching (no parallelism)
- No proof prefetching
- No compression for proof batches

**Solution**: Optimize fast sync:
- Parallel proof fetching (up to 8 concurrent)
- Proof prefetching (fetch N blocks ahead)
- Proof batch compression (zstd)
- Progress reporting for UX

### 5. Monitoring & Observability (Operations)
**Problem**: No visibility into Utreexo subsystem health:
- No metrics for proof generation time
- No stats on proof cache efficiency
- No tracking of stateless sync progress
- No alerts for proof unavailability

**Solution**: Comprehensive metrics:
- Prometheus-style metrics export
- Per-RPC latency histograms
- Proof cache hit rate
- Stateless sync progress/ETA
- Proof unavailability alerts

---

## Components

### 11a.1: Batch Proof RPCs

**New RPCs** (`src/rpc/methods_utreexo_batch.cpp`):

```cpp
/**
 * blockchain.getutxoproofs_batch - Generate proofs for multiple UTXOs
 *
 * Params:
 *   [0] utxos (array, required): Array of {txid, vout} objects
 *
 * Returns:
 *   {
 *     "proofs": [
 *       {
 *         "txid": "<txid>",
 *         "vout": <n>,
 *         "proof": {...},
 *         "proof_size": <n>
 *       },
 *       ...
 *     ],
 *     "batch_size": <n>,
 *     "generation_time_ms": <n>
 *   }
 */
Json rpc_getutxoproofs_batch(const ExecutionContext& ctx, const Json& params);

/**
 * blockchain.verifyutxoproofs_batch - Verify multiple proofs
 *
 * Params:
 *   [0] proofs (array, required): Array of proof objects from getutxoproofs_batch
 *
 * Returns:
 *   {
 *     "results": [
 *       {"txid": "<txid>", "vout": <n>, "valid": true},
 *       {"txid": "<txid>", "vout": <n>, "valid": false, "error": "..."},
 *       ...
 *     ],
 *     "batch_size": <n>,
 *     "verification_time_ms": <n>
 *   }
 */
Json rpc_verifyutxoproofs_batch(const ExecutionContext& ctx, const Json& params);
```

**Performance Target**:
- Batch size: 1-1000 UTXOs
- Generation: <1ms per proof (1000 proofs in ~1 second)
- Verification: <0.5ms per proof (2000 proofs in ~1 second)

**Tests**: `tests/rpc/test_utreexo_batch_rpcs.cpp`

---

### 11a.2: Proof Cache Service

**New Service** (`src/daemon/services/proof_cache_service.h`):

```cpp
namespace daemon {

class ProofCacheService : public Service {
public:
    struct Config {
        size_t max_proofs = 10000;  // Max cached proofs
        uint32_t ttl_blocks = 6;    // Cache valid for 6 blocks (~1 hour)
        bool enabled = true;
    };

    explicit ProofCacheService(Config config = Config{});

    // Service interface
    std::string Name() const override { return "ProofCache"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Cache operations
    void CacheProof(
        const uint256& txid,
        uint32_t vout,
        const consensus::UtreexoProof& proof,
        uint32_t block_height
    );

    std::optional<consensus::UtreexoProof> GetProof(
        const uint256& txid,
        uint32_t vout,
        uint32_t current_height
    );

    void InvalidateOldEntries(uint32_t new_block_height);
    void Clear();

    // Metrics
    struct Stats {
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t evictions = 0;
        size_t current_size = 0;
        double hit_rate = 0.0;
    };
    Stats GetStats() const;

private:
    Config config_;

    struct CacheEntry {
        consensus::UtreexoProof proof;
        uint32_t cached_at_height;
    };

    // (txid, vout) -> (proof, height)
    std::unordered_map<std::pair<uint256, uint32_t>, CacheEntry> cache_;
    std::list<std::pair<uint256, uint32_t>> lru_list_;  // LRU eviction

    Stats stats_;
    mutable std::mutex mutex_;
};

} // namespace daemon
```

**Integration**:
- Wire into `daemon_app.cpp` after chainstate initialization
- Hook into `getutxoproof` RPC for transparent caching
- Invalidate on new blocks via chainstate event

**Tests**: `tests/daemon/test_proof_cache_service.cpp`

---

### 11a.3: Stateless Mode Hardening

**Improvements** (`src/network/stateless_node.cpp`):

```cpp
class StatelessNode {
public:
    struct Config {
        bool enable_multi_peer_fetch = true;  // Fetch from multiple peers
        uint32_t max_proof_retries = 3;       // Retry failed requests
        uint32_t retry_backoff_ms = 1000;     // Exponential backoff base
        size_t max_concurrent_fetches = 8;    // Parallel proof fetches
    };

    // Enhanced proof fetching with fallback
    std::optional<consensus::UtreexoProof> FetchProofWithRetry(
        const uint256& txid,
        uint32_t vout,
        const std::vector<std::string>& peer_candidates
    );

    // Health check
    struct HealthStatus {
        bool operational;
        size_t successful_fetches;
        size_t failed_fetches;
        std::vector<std::string> available_peers;
        std::string status_message;
    };
    HealthStatus GetHealthStatus() const;

    // Metrics
    struct Metrics {
        uint64_t proofs_requested = 0;
        uint64_t proofs_fetched = 0;
        uint64_t proofs_failed = 0;
        uint64_t retries_performed = 0;
        double fetch_success_rate = 0.0;
        double avg_fetch_time_ms = 0.0;
    };
    Metrics GetMetrics() const;
};
```

**Features**:
- Multi-peer proof fetching (try 3 peers before giving up)
- Exponential backoff for retries (1s, 2s, 4s)
- Graceful degradation (warn user, don't crash)
- Detailed logging for debugging

**Tests**: `tests/network/test_stateless_node_hardening.cpp`

---

### 11a.4: Fast Sync Optimizations

**Enhancements** (`src/daemon/services/fast_sync_service.cpp`):

```cpp
class FastSyncService : public Service {
public:
    struct Config {
        size_t max_parallel_fetches = 8;   // Concurrent proof fetches
        size_t prefetch_blocks = 100;      // Prefetch N blocks ahead
        bool enable_compression = true;    // Compress proof batches
        bool enable_progress_ui = true;    // Show progress bar
    };

    // Parallel proof fetching
    std::vector<consensus::BlockUtreexoData> FetchProofBatchParallel(
        const std::vector<uint256>& block_hashes
    );

    // Proof prefetching
    void PrefetchProofsAsync(
        uint32_t start_height,
        uint32_t end_height
    );

    // Progress reporting
    struct SyncProgress {
        uint32_t current_height;
        uint32_t target_height;
        double percent_complete;
        uint64_t eta_seconds;
        double blocks_per_second;
    };
    SyncProgress GetProgress() const;
};
```

**Optimizations**:
- Parallel proof fetching: 8 concurrent requests (8x speedup)
- Proof prefetching: Fetch block N+100 while validating block N
- Proof batch compression: zstd (50-70% size reduction)
- Progress UI: ETA and blocks/sec for user feedback

**Tests**: `tests/daemon/test_fast_sync_optimizations.cpp`

---

### 11a.5: Monitoring & Metrics

**New Service** (`src/daemon/services/utreexo_metrics_service.h`):

```cpp
namespace daemon {

class UtreexoMetricsService : public Service {
public:
    std::string Name() const override { return "UtreexoMetrics"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Record metrics
    void RecordProofGeneration(uint64_t duration_us);
    void RecordProofVerification(uint64_t duration_us);
    void RecordCacheHit();
    void RecordCacheMiss();
    void RecordStatelessFetch(bool success, uint64_t duration_us);

    // Export metrics (Prometheus format)
    std::string ExportPrometheusMetrics() const;

    // Get metrics
    struct Metrics {
        // Proof generation
        uint64_t proofs_generated = 0;
        double avg_proof_gen_time_us = 0.0;
        double p50_proof_gen_time_us = 0.0;
        double p99_proof_gen_time_us = 0.0;

        // Proof cache
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        double cache_hit_rate = 0.0;

        // Stateless sync
        uint64_t stateless_fetches = 0;
        uint64_t stateless_failures = 0;
        double stateless_success_rate = 0.0;
        double avg_fetch_time_us = 0.0;

        // Accumulator stats
        uint64_t num_leaves = 0;
        uint64_t num_roots = 0;
        size_t forest_size_bytes = 0;
    };
    Metrics GetMetrics() const;

private:
    Metrics metrics_;
    std::vector<uint64_t> proof_gen_times_;  // For percentiles
    std::vector<uint64_t> fetch_times_;
    mutable std::mutex mutex_;
};

} // namespace daemon
```

**Metrics Exported**:
- `dinero_utreexo_proofs_generated_total` - Total proofs generated
- `dinero_utreexo_proof_gen_time_us_bucket` - Latency histogram
- `dinero_utreexo_cache_hit_rate` - Cache efficiency
- `dinero_utreexo_stateless_fetch_success_rate` - Stateless reliability
- `dinero_utreexo_forest_leaves` - Current accumulator size

**Integration**: HTTP endpoint `/metrics` for Prometheus scraping

**Tests**: `tests/daemon/test_utreexo_metrics.cpp`

---

## Implementation Plan

### Week 1: Batch RPCs (11a.1)
- [ ] Implement `getutxoproofs_batch`
- [ ] Implement `verifyutxoproofs_batch`
- [ ] Add performance tests (1000+ proofs/sec)
- [ ] Update RPC documentation

**Exit Criteria**: Batch RPCs handle 1000 proofs in <1 second

---

### Week 2: Proof Cache Service (11a.2)
- [ ] Implement `ProofCacheService`
- [ ] Wire into `daemon_app.cpp`
- [ ] Hook into existing `getutxoproof` RPC
- [ ] Add cache invalidation on new blocks
- [ ] Test cache hit rate (>80% for repeated queries)

**Exit Criteria**: Cache reduces proof generation latency by 10x for repeated queries

---

### Week 3: Stateless Mode Hardening (11a.3)
- [ ] Add multi-peer proof fetching
- [ ] Implement retry logic with backoff
- [ ] Add health checks and metrics
- [ ] Test graceful degradation scenarios

**Exit Criteria**: Stateless node survives 50% peer failures without crashing

---

### Week 4: Fast Sync Optimizations (11a.4)
- [ ] Implement parallel proof fetching
- [ ] Add proof prefetching
- [ ] Integrate zstd compression
- [ ] Add progress reporting UI
- [ ] Benchmark sync speed improvement

**Exit Criteria**: Fast sync 5x faster than sequential baseline

---

### Week 5: Monitoring & Metrics (11a.5)
- [ ] Implement `UtreexoMetricsService`
- [ ] Export Prometheus metrics
- [ ] Add `/metrics` HTTP endpoint
- [ ] Create Grafana dashboard template
- [ ] Document all metrics

**Exit Criteria**: Grafana dashboard shows all Utreexo subsystem metrics

---

### Week 6: Integration & Testing
- [ ] Full regression testing (Phases 8, 9, 10, 10e)
- [ ] Performance benchmarking
- [ ] Update documentation
- [ ] Create operational runbook

**Exit Criteria**: All tests pass, no regressions, documentation complete

---

## Success Criteria

**Phase 11a is complete when**:

### Functional Requirements
1. ✅ Batch RPCs handle 1000+ proofs/second
2. ✅ Proof cache reduces latency by 10x
3. ✅ Stateless mode survives peer failures gracefully
4. ✅ Fast sync is 5x faster with parallelism
5. ✅ Metrics exported via `/metrics` endpoint

### Performance Requirements
6. ✅ `getutxoproofs_batch`: 1000 proofs in <1 second
7. ✅ Proof cache hit rate: >80% for repeated queries
8. ✅ Stateless sync success rate: >95% with 3 peers
9. ✅ Fast sync: <10 minutes for 10,000 blocks (stateless mode)

### Operational Requirements
10. ✅ Zero regressions in existing tests
11. ✅ Prometheus metrics scraping works
12. ✅ Grafana dashboard template provided
13. ✅ Operational runbook documented

### Code Quality Requirements
14. ✅ All new code in appropriate namespaces
15. ✅ No consensus modifications
16. ✅ Thread-safe service implementations
17. ✅ Comprehensive error handling

---

## Testing Strategy

### Unit Tests
- `tests/rpc/test_utreexo_batch_rpcs.cpp` - Batch RPC tests
- `tests/daemon/test_proof_cache_service.cpp` - Cache tests
- `tests/network/test_stateless_node_hardening.cpp` - Stateless tests
- `tests/daemon/test_fast_sync_optimizations.cpp` - Fast sync tests
- `tests/daemon/test_utreexo_metrics.cpp` - Metrics tests

### Integration Tests
- `tests/test_phase11a_e2e.sh` - End-to-end Phase 11a tests
- `tests/test_stateless_sync_resilience.sh` - Peer failure scenarios
- `tests/test_fast_sync_performance.sh` - Sync speed benchmarks

### Performance Tests
- Batch RPC throughput: 1000 proofs/sec minimum
- Cache hit rate: >80% for repeated queries
- Fast sync speed: 5x improvement over baseline

### Regression Tests
- All Phase 8, 9, 10, 10e tests must continue passing
- No performance regressions in existing RPCs
- No memory leaks in long-running tests

---

## Migration Path

**Phase 11a is backwards-compatible**:
- ✅ Existing RPCs unchanged (`getutxoproof` still works)
- ✅ New RPCs are additive (`getutxoproofs_batch` is opt-in)
- ✅ Proof cache is transparent (no API changes)
- ✅ Stateless mode improvements are internal
- ✅ Metrics are optional (disabled by default)

**Deployment**:
1. Deploy Phase 11a binaries
2. No configuration changes required
3. Optionally enable metrics: `--enable-utreexo-metrics`
4. Optionally use batch RPCs for performance

**Rollback**:
- Revert to Phase 10e if issues found
- No data migration needed (cache is in-memory)

---

## Security Considerations

### Proof Cache DoS Protection
- **Attack**: Adversary floods cache with fake proofs
- **Mitigation**: LRU eviction, max size limit (10,000 proofs), TTL expiration

### Stateless Peer Reputation
- **Attack**: Malicious peers send invalid proofs
- **Mitigation**: Cryptographic verification (Phase 8), peer banning on repeated failures

### Batch RPC Resource Limits
- **Attack**: Adversary requests 1,000,000 proofs in one call
- **Mitigation**: Max batch size (1,000 proofs), rate limiting

### Metrics Information Disclosure
- **Attack**: Adversary scrapes `/metrics` to fingerprint node
- **Mitigation**: Require authentication for metrics endpoint, or disable by default

---

## Documentation Deliverables

1. **RPC Documentation** (`docs/rpc/utreexo_batch.md`)
   - Batch RPC examples and use cases
   - Performance tuning guide

2. **Operator Guide** (`docs/operations/utreexo_operations.md`)
   - Configuring proof cache
   - Running stateless nodes
   - Troubleshooting proof fetch failures

3. **Metrics Reference** (`docs/metrics/utreexo_metrics.md`)
   - All exported metrics with descriptions
   - Grafana dashboard JSON
   - Alerting rules

4. **Performance Tuning** (`docs/performance/utreexo_tuning.md`)
   - Proof cache sizing
   - Parallel fetch tuning
   - Fast sync optimization tips

---

## Tag & Milestone

**Tag**: `phase-11a-proof-ops`
**Message**: "Phase 11a: Utreexo proof operations hardening - batch RPCs, caching, stateless improvements"

**Milestone**: `v0.16.0` - Utreexo Production Hardening

---

## References

- **Phase 7.4.3**: Stateless mode introduction
- **Phase 8**: Stateless validation framework
- **Phase 9**: Proof distribution infrastructure
- **Phase 10e**: Utreexo mining finalized
- **Existing RPCs**: `src/rpc/methods_utreexo.cpp`
- **Utreexo Paper**: https://eprint.iacr.org/2019/611

---

**Phase 11a Status**: Design complete, ready for approval 📋
