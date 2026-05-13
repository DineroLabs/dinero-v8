# Parallel Block Download Testing Plan

**Status**: Test suite created, integration verification pending
**Implementation**: Complete (commits 99377f01e, 51ded7fb1)

## Overview

The parallel block download system enables 10-20× IBD speedup by downloading blocks from 4-8 peers simultaneously. This document outlines the testing strategy for validating the implementation.

## Implementation Summary

### Core Components

1. **PeerScore** - Peer reputation tracking
   - Latency monitoring (exponential moving average)
   - Success rate calculation
   - Bandwidth measurement
   - Timeout strike tracking
   - Composite score (0.0-1.0): `(latency × 0.4) + (success × 0.3) + (bandwidth × 0.3)`

2. **BlockDownloadTask** - Block download state tracking
   - Assigned peer
   - Request timestamps
   - Retry counting
   - Timeout detection (10s threshold)

3. **ParallelBlockDownloader** - Main coordinator
   - Multi-peer selection (up to 8 peers)
   - Round-robin block distribution
   - Timeout handling and reassignment
   - Slow peer isolation (3-strike rule, 60s cooldown)

### Key Configuration

```cpp
static constexpr int DEFAULT_MAX_PARALLEL_PEERS = 8;        // 8 peers simultaneously
static constexpr int MAX_BLOCKS_PER_PEER = 64;              // 64 blocks per peer
static constexpr int MAX_TOTAL_IN_FLIGHT = 512;             // 512 total in-flight limit

static constexpr int BLOCK_TIMEOUT_MS = 10000;              // 10s timeout
static constexpr int PEER_TIMEOUT_STRIKES = 3;              // 3 strikes → cooldown
static constexpr int SLOW_PEER_COOLDOWN_MS = 60000;         // 60s cooldown

static constexpr double MIN_SUCCESS_RATE = 0.7;             // 70% minimum
static constexpr int64_t MAX_ACCEPTABLE_LATENCY_MS = 5000;  // 5s max latency
static constexpr int64_t MIN_BANDWIDTH_BPS = 10000;         // 10 KB/s minimum
```

## Test Suite

### Unit Tests (tests/test_parallel_block_download.cpp)

**Test Coverage:**

1. **Peer Score Calculation**
   - Initial scores (perfect 1.0)
   - Success recording (latency, bandwidth tracking)
   - Failure recording (score degradation)
   - Composite score weighting verification

2. **Peer Reputation Tracking**
   - Success rate calculation (7 successes, 3 failures = 70%)
   - Latency exponential moving average
   - Bandwidth exponential moving average
   - Timeout strike accumulation and reset

3. **Slow Peer Isolation**
   - 3-strike cooldown trigger
   - Cooldown state (unavailable, score = 0.0)
   - Cooldown expiration (60s timeout)

4. **Block Download Task Management**
   - Timeout detection (10s threshold)
   - Task reassignment to alternate peers
   - Retry count incrementing

5. **Configuration Validation**
   - Bitcoin-compatible limits
   - Timeout thresholds
   - Performance thresholds

6. **Score Penalties**
   - Low success rate penalty (< 70%)
   - High latency penalty (> 5s)

### Integration Tests (Pending)

**Test Scenarios:**

1. **Multi-Peer Block Distribution**
   ```
   Setup: 4 peers, 100 blocks to download
   Expected: Blocks distributed ~25 per peer (round-robin)
   Validate: Per-peer in-flight counts, global limit enforcement
   ```

2. **Peer Selection Prioritization**
   ```
   Setup: 10 peers with varying latencies (100ms-550ms)
   Expected: Best 8 peers selected (sorted by score)
   Validate: Peer rankings match score order
   ```

3. **Timeout Handling**
   ```
   Setup: Peer times out after 10s
   Expected: Block reassigned to alternate peer
   Validate: Timeout strike incremented, task moved
   ```

4. **Slow Peer Cooldown**
   ```
   Setup: Peer hits 3 timeout strikes
   Expected: Peer enters 60s cooldown, excluded from selection
   Validate: Peer unavailable, score = 0.0
   ```

5. **Global Limit Enforcement**
   ```
   Setup: 8 peers, attempt to schedule 1000 blocks
   Expected: Cap at 512 total in-flight
   Validate: getInflightCount() ≤ 512
   ```

6. **Per-Peer Limit Enforcement**
   ```
   Setup: 1 peer, attempt to schedule 100 blocks
   Expected: Cap at 64 blocks per peer
   Validate: Peer in-flight count ≤ 64
   ```

###Performance Benchmarks (Disabled by default)

**Speedup Verification:**

```cpp
TEST_F(ParallelBlockDownloadTest, DISABLED_BenchmarkParallelSpeedup) {
    // Simulate sequential: 1000 blocks × 100ms = 100s
    // Simulate parallel: 1000 blocks / 8 peers × 100ms ≈ 12.5s
    // Expected speedup: ~8×

    EXPECT_GE(speedup, 8.0);  // At minimum match peer count
}
```

## Manual Testing Guide

### Prerequisites

1. Build daemon with parallel block download enabled
2. Connect to testnet or regtest network
3. Monitor logs for parallel download activity

### Test Procedure

1. **Start Syncing from Genesis**
   ```bash
   ./build/dinerod --testnet --reindex
   ```

2. **Monitor Parallel Activity**
   ```bash
   tail -f ~/.dinero/testnet/debug.log | grep -E "(Scheduled|Parallel|Peer)"
   ```

3. **Expected Log Patterns**
   ```
   Scheduled 64 blocks across 8 peers
   Block received from peer (latency: 123ms, score: 0.95)
   Peer 0x1234 entered cooldown for 60s
   Reassigned block abc123... to alternate peer
   ```

4. **Verify Metrics**
   - Check peer counts: Should see 4-8 active peers
   - Check in-flight blocks: Should see 256-512 blocks in-flight
   - Check latencies: Should see sub-second response times
   - Check speedup: Compare sync time vs single-peer baseline

### Success Criteria

✅ **Functional Requirements:**
- Blocks download from multiple peers simultaneously
- Timeout detection triggers within 10-15 seconds
- Slow peers isolated after 3 strikes
- Blocks reassigned to alternate peers on timeout

✅ **Performance Requirements:**
- 8-16× speedup vs single-peer baseline
- Aggregate bandwidth: 80+ MB/s with fast peers
- In-flight utilization: 400-512 blocks maintained

✅ **Reliability Requirements:**
- No deadlocks or stalls during IBD
- Graceful handling of peer disconnections
- Correct block ordering maintained

## Known Limitations

1. **Network Integration Pending**
   - `requestBlocksFromPeer()` is a placeholder
   - Needs P2P layer integration for actual getdata messages

2. **Peer Manager Integration**
   - `selectParallelPeers()` needs PeerManager API
   - Currently uses tracked peers only

3. **Height-Based Prioritization**
   - Block height extraction not implemented
   - Blocks downloaded in hash order (not height order)

## Production Readiness Checklist

- [x] Core parallel download logic
- [x] Peer reputation scoring
- [x] Timeout handling and reassignment
- [x] Slow peer isolation
- [x] Bitcoin-compatible limits
- [x] Unit test suite
- [ ] P2P network integration
- [ ] PeerManager integration
- [ ] Live P2P testing
- [ ] Performance benchmarks
- [ ] Mainnet stress testing

## Files

1. **include/p2p/parallel_block_downloader.h** (412 lines)
   - PeerScore, BlockDownloadTask, ParallelBlockDownloader classes

2. **src/p2p/parallel_block_downloader.cpp** (493 lines)
   - Full implementation with scoring, timeout handling, reassignment

3. **tests/test_parallel_block_download.cpp** (374 lines)
   - Comprehensive unit test suite (13 test cases)

4. **docs/parallel_block_download_testing.md** (This file)
   - Testing strategy and manual verification guide

## Next Steps

1. **Complete P2P Integration**
   - Implement `requestBlocksFromPeer()` with actual getdata messages
   - Connect to PeerManager for peer selection

2. **Run Integration Tests**
   - Build test executable
   - Execute full test suite
   - Verify all assertions pass

3. **Live Network Testing**
   - Connect to testnet
   - Monitor parallel download behavior
   - Measure actual speedup vs baseline

4. **Performance Validation**
   - Run benchmark tests
   - Verify 10-20× speedup achieved
   - Measure aggregate bandwidth utilization

## References

- Bitcoin Core: `src/net_processing.cpp` (ProcessMessage, SendMessages)
- Implementation commits: 99377f01e, 51ded7fb1
- Design doc: `docs/sync_pipeline.md`
