# DineroCoin - Cooperative CPU+GPU Mining Design

## Overview

This document describes the dual-mode mining architecture where CPU and GPU work cooperatively on the same block template with disjoint nonce ranges, maximizing hash power without wasted effort.

## Architecture

```
         ┌────────────────────────────┐
         │      Mining Manager         │
         │────────────────────────────│
         │  getBlockTemplate()        │
         │  distributeWork()          │
         │  collectResults()          │
         └──────────┬─────────────────┘
                    │
   ┌────────────────┼────────────────┐
   │                                 │
CPUWorkerPool                   GPUDeviceManager
 (N threads)                     (M devices)
   │                                 │
   ▼                                 ▼
 HashLoop()                   KernelLaunch()
 nonce range:                 nonce range:
 [0 .. N*batch]               [N*batch+1 .. MAX]
   │                                 │
   └───────►  onSolutionFound() ◄──────┘
                  ↓
         submitBlock → blockchain
```

## Key Principles

### 1. Disjoint Nonce Ranges
- **CPU Backend**: Gets nonce range `[0, cpu_batch_size]`
- **GPU Backend**: Gets nonce range `[cpu_batch_size+1, uint32_max]`
- No overlap, no wasted hashing power

### 2. Shared Block Template
Both backends work on identical:
- Block header (80 bytes)
- Target difficulty
- Merkle root
- Timestamp

### 3. First-Valid-Solution Wins
- Both backends report to shared callback
- Mutex protects submission
- First valid solution cancels other work
- Block submitted immediately

### 4. Independent Threads
- CPU: `N` worker threads in pool
- GPU: `M` device threads (one per GPU)
- No blocking between backends

## Data Structures

### WorkPackage (Enhanced)
```cpp
struct WorkPackage {
    uint8_t header[80];        // Block header
    uint256 target;            // Difficulty target
    uint32_t nonce_start;      // Start of nonce range
    uint32_t nonce_end;        // End of nonce range (inclusive)
    BackendType backend;       // CPU or GPU
    uint32_t device_id;        // For multi-GPU
};
```

### MiningResult
```cpp
struct MiningResult {
    bool found;                // Solution found?
    uint32_t nonce;            // Winning nonce
    uint256 hash;              // Resulting hash
    uint64_t hashes_tried;     // Total attempts
    BackendType backend;       // Which backend found it
};
```

### MiningStatistics
```cpp
struct MiningStatistics {
    double cpu_hashrate;       // CPU H/s
    double gpu_hashrate;       // GPU H/s
    double total_hashrate;     // Combined H/s
    uint64_t cpu_hashes;       // Total CPU hashes
    uint64_t gpu_hashes;       // Total GPU hashes
    int blocks_found_cpu;      // Blocks found by CPU
    int blocks_found_gpu;      // Blocks found by GPU
};
```

## Work Distribution Algorithm

### Static Distribution (Phase 1)
```cpp
const uint32_t CPU_NONCE_RANGE = 10'000'000;  // 10M nonces for CPU
const uint32_t GPU_NONCE_START = CPU_NONCE_RANGE + 1;

WorkPackage cpu_work {
    .header = block_header,
    .target = difficulty_target,
    .nonce_start = 0,
    .nonce_end = CPU_NONCE_RANGE,
    .backend = BackendType::CPU
};

WorkPackage gpu_work {
    .header = block_header,  // Same header!
    .target = difficulty_target,
    .nonce_start = GPU_NONCE_START,
    .nonce_end = UINT32_MAX,
    .backend = BackendType::GPU
};
```

### Dynamic Distribution (Phase 2 - Future)
Adjust ranges based on relative hashrate:
```cpp
double cpu_ratio = cpu_hashrate / (cpu_hashrate + gpu_hashrate);
uint32_t cpu_range = (uint32_t)(UINT32_MAX * cpu_ratio);
```

## Solution Submission

### Thread-Safe Callback
```cpp
class MiningManager {
private:
    std::mutex solution_mutex_;
    std::atomic<bool> solution_submitted_{false};

    void onSolutionFound(const MiningResult& result) {
        std::lock_guard<std::mutex> lock(solution_mutex_);

        if (solution_submitted_.load()) {
            return;  // Already submitted
        }

        // Verify solution
        if (!verifySolution(result)) {
            g_logger.error("[Mining] Invalid solution rejected");
            return;
        }

        // Submit block
        submitBlock(current_job_, result.nonce);
        solution_submitted_.store(true);

        // Signal all backends to stop work on this template
        current_job_->stop_mining.store(true);

        // Log success
        const char* backend_name = (result.backend == BackendType::CPU) ? "CPU" : "GPU";
        g_logger.info("[Mining] Block found by " + std::string(backend_name) +
                      " at nonce 0x" + toHex(result.nonce));
    }
};
```

## Performance Monitoring

### Hashrate Calculation
```cpp
void MiningManager::updateHashrate() {
    auto now = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(now - last_update_).count();

    if (elapsed < 1000) return;  // Update every second

    // CPU hashrate
    uint64_t cpu_delta = cpu_hashes_.exchange(0);
    cpu_hashrate_ = (cpu_delta * 1000.0) / elapsed;

    // GPU hashrate
    uint64_t gpu_delta = gpu_hashes_.exchange(0);
    gpu_hashrate_ = (gpu_delta * 1000.0) / elapsed;

    // Total
    total_hashrate_ = cpu_hashrate_ + gpu_hashrate_;

    last_update_ = now;
}
```

## RPC Interface Updates

### mining.info (Enhanced)
```json
{
  "is_mining": true,
  "thread_count": 12,
  "cpu_hashrate": 2100000,
  "gpu_hashrate": 56000000,
  "total_hashrate": 58100000,
  "gpu_enabled": true,
  "gpu_device_count": 1,
  "gpu_device_name": "NVIDIA RTX 4070",
  "blocks_found": 42,
  "blocks_found_cpu": 5,
  "blocks_found_gpu": 37
}
```

### mining.gpuinfo
```json
{
  "gpu_mining_active": true,
  "gpu_available": true,
  "gpu_hashrate": 56000000,
  "active_gpus": 1,
  "gpus": [
    {
      "device_id": 0,
      "name": "NVIDIA RTX 4070",
      "hashrate": 56000000,
      "backend": "CUDA",
      "nonce_range_start": 10000001,
      "nonce_range_end": 4294967295
    }
  ]
}
```

## Configuration

### Consensus Parameters
```cpp
struct ConsensusParams {
    bool allowGPUMining;              // Enable GPU mining
    uint32_t gpuMiningActivationHeight;  // Block height to enable
    uint32_t cpuNonceRange;           // Static range for CPU (default: 10M)
    bool dynamicWorkDistribution;      // Enable dynamic adjustment
};
```

### Mining Config
```cpp
struct MiningConfig {
    int cpu_threads;              // Number of CPU threads
    bool enable_gpu;              // Enable GPU mining
    std::vector<int> gpu_devices; // GPU device IDs to use
    uint32_t work_refresh_ms;     // Template refresh interval
    uint32_t nonce_batch_size;    // Nonces per CPU iteration
};
```

## Error Handling

### GPU Failure Scenarios
1. **GPU initialization fails**: Continue with CPU-only mining
2. **GPU crashes during mining**: Restart GPU backend, continue CPU
3. **GPU hangs**: Detect timeout, kill GPU thread, restart
4. **Driver issues**: Fall back to CPU-only mode

### Recovery Strategy
```cpp
void MiningManager::handleGPUFailure() {
    g_logger.warning("[GPU] GPU backend failed, falling back to CPU-only");

    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
    }

    gpu_enabled_.store(false);

    // Redistribute full nonce range to CPU
    redistributeWorkToCPU();
}
```

## Testing Strategy

### Unit Tests
- [ ] Nonce range distribution (no overlap)
- [ ] Solution callback mutex protection
- [ ] Hashrate calculation accuracy
- [ ] Work template sharing

### Integration Tests
- [ ] CPU+GPU cooperative mining
- [ ] Solution submission from GPU
- [ ] Solution submission from CPU
- [ ] Simultaneous solutions (race condition)
- [ ] GPU failure recovery

### Performance Tests
- [ ] CPU hashrate measurement
- [ ] GPU hashrate measurement
- [ ] Combined hashrate = sum of parts
- [ ] No performance degradation from coordination overhead

## Implementation Phases

### Phase 1: Static Range Distribution ✅ CURRENT
- Fixed nonce ranges
- Simple work distribution
- Basic cooperative mining

### Phase 2: Dynamic Range Distribution (Future)
- Hashrate-based adjustment
- Adaptive work splitting
- Performance optimization

### Phase 3: Multi-GPU Support (Future)
- Multiple GPU devices
- Per-device nonce ranges
- Load balancing across GPUs

### Phase 4: Stratum Integration (Future)
- Pool mining support
- Share submission
- External work source

## Benefits

1. **No Wasted Hashing**: Disjoint ranges eliminate duplicate work
2. **Maximum Performance**: Full utilization of CPU + GPU
3. **Simplicity**: Straightforward architecture
4. **Reliability**: Independent backends, graceful degradation
5. **Scalability**: Easy to add more GPUs or CPU threads

## Expected Performance

### Example System
- **CPU**: AMD Ryzen 9 5950X (16 cores) = ~2.1 MH/s
- **GPU**: NVIDIA RTX 4070 = ~56 MH/s
- **Total**: ~58.1 MH/s (combined)
- **Efficiency**: ~97% (minimal overhead from coordination)

### Block Finding Time Reduction
At difficulty 1024:
- CPU alone: ~7.4 minutes per block
- CPU+GPU: ~16 seconds per block
- **27x faster** with GPU acceleration
