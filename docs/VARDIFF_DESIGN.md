# Vardiff (Variable Difficulty) Implementation Design

## Overview

Dynamic difficulty adjustment for Stratum V1 mining pool to optimize share submission frequency and prevent server overload.

## Goals

1. **Target Share Rate**: 1 share per 15 seconds per miner
2. **Smooth Adjustment**: Use EWMA (Exponentially Weighted Moving Average) to avoid oscillation
3. **Per-Worker**: Each miner gets individual difficulty based on their hashrate
4. **Scalable**: Support many concurrent miners without performance degradation

## Algorithm

### 1. Hashrate Estimation (EWMA)

```
For each share submission:
  time_delta = current_time - last_share_time
  estimated_hashrate = difficulty * 2^32 / time_delta

  if first_share:
    hashrate_ewma = estimated_hashrate
  else:
    alpha = 0.1  // Smoothing factor (0.1 = recent 10% weight)
    hashrate_ewma = alpha * estimated_hashrate + (1 - alpha) * hashrate_ewma
```

### 2. Difficulty Adjustment

```
target_time = 15 seconds
new_difficulty = hashrate_ewma * target_time / 2^32

Constraints:
  min_difficulty = 1.0
  max_difficulty = 10000.0
  max_adjustment_factor = 4.0  // Don't change by more than 4x at once

  if new_difficulty < min_difficulty:
    new_difficulty = min_difficulty
  if new_difficulty > max_difficulty:
    new_difficulty = max_difficulty
  if new_difficulty > current_difficulty * max_adjustment_factor:
    new_difficulty = current_difficulty * max_adjustment_factor
  if new_difficulty < current_difficulty / max_adjustment_factor:
    new_difficulty = current_difficulty / max_adjustment_factor
```

### 3. Adjustment Trigger

Adjust difficulty when:
- **Time-based**: Every N shares (e.g., every 8 shares)
- **Deviation-based**: When share rate deviates >30% from target

```
if shares_since_last_adjustment >= 8:
  avg_time_per_share = total_time / shares_since_last_adjustment

  if abs(avg_time_per_share - target_time) / target_time > 0.3:
    adjust_difficulty()
```

## Data Structure Changes

### ClientSession (Per-Worker State)

```cpp
struct ClientSession {
    // Existing fields
    std::string worker_name;
    std::string extra_nonce1;
    double difficulty;
    bool authorized;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    std::chrono::steady_clock::time_point connected_at;

    // NEW: Vardiff state
    double hashrate_ewma;  // EWMA-smoothed hashrate estimate (H/s)
    std::chrono::steady_clock::time_point last_share_time;
    uint64_t shares_since_adjustment;
    std::chrono::steady_clock::time_point last_adjustment_time;
    double total_time_since_adjustment;  // For average calculation
};
```

## Implementation Steps

### Phase 1: Hashrate Tracking
1. Add Vardiff state to `ClientSession`
2. Track share submission timing
3. Calculate EWMA hashrate on each share

### Phase 2: Difficulty Adjustment
1. Implement adjustment algorithm with constraints
2. Trigger adjustment based on share count and deviation
3. Update `ClientSession::difficulty` per worker

### Phase 3: Broadcasting
1. Call `handleSetDifficulty()` to send `mining.set_difficulty`
2. Log adjustment events for monitoring
3. Update RPC statistics to include per-worker difficulty

## Configuration Parameters

```cpp
struct VardiffConfig {
    double target_time = 15.0;           // Target seconds per share
    double min_difficulty = 1.0;         // Minimum difficulty
    double max_difficulty = 10000.0;     // Maximum difficulty
    double ewma_alpha = 0.1;             // EWMA smoothing factor
    double max_adjustment_factor = 4.0;  // Max 4x change per adjustment
    uint64_t shares_per_adjustment = 8;  // Adjust every N shares
    double deviation_threshold = 0.3;    // 30% deviation triggers adjustment
};
```

## Example Scenarios

### Scenario 1: Slow Miner (CPU)
```
Initial: difficulty = 1.0
Shares: 25s, 28s, 22s, 30s, 26s, 24s, 27s, 29s (avg 26.375s)
EWMA hashrate: ~163 MH/s
Target: 15s per share
New difficulty: 0.57 → clamped to 1.0 (min)
Result: No adjustment (already at minimum)
```

### Scenario 2: Fast Miner (GPU)
```
Initial: difficulty = 1.0
Shares: 3s, 2.8s, 3.2s, 2.9s, 3.1s, 3.0s, 2.9s, 3.1s (avg 3.0s)
EWMA hashrate: ~1430 MH/s
Target: 15s per share
New difficulty: 5.0
Result: Adjust to 5.0, broadcast mining.set_difficulty
```

### Scenario 3: Very Fast Miner (ASIC Farm)
```
Initial: difficulty = 1.0
Shares: 0.2s, 0.18s, 0.21s, 0.19s, 0.20s, 0.19s, 0.21s, 0.20s (avg 0.198s)
EWMA hashrate: ~21700 MH/s
Target: 15s per share
New difficulty: 75.8
Adjustment limit: 4x → adjust to 4.0 first iteration
Next iteration: 4.0 → 16.0 → 64.0 → 75.8 (3 iterations to converge)
Result: Gradual ramp-up to prevent shock
```

## Testing Strategy

1. **Unit Tests**: EWMA calculation, difficulty adjustment logic
2. **Protocol Tests**: Verify `mining.set_difficulty` broadcast
3. **Load Tests**: 100+ concurrent miners with varying hashrates
4. **Monitoring**: RPC endpoint shows per-worker difficulty and hashrate

## Performance Considerations

- **Memory**: ~100 bytes per worker (negligible)
- **CPU**: O(1) per share submission (timestamp diff + EWMA update)
- **Lock contention**: Per-worker state (no global lock needed)
- **Scalability**: Supports 1000+ concurrent miners

## Future Enhancements (Out of Scope for P3)

- **Adaptive EWMA**: Adjust alpha based on miner stability
- **Difficulty presets**: Fast-start for known worker IDs
- **Multi-algo**: Different targets for different hash algorithms
- **Stratum V2**: Extended job negotiation protocol

---

**Status**: Design Complete
**Implementation**: Ready to Begin
**Target**: P3 (Vardiff Implementation)
