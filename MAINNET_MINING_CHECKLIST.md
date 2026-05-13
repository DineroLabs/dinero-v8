# 🔒 Mainnet Mining Readiness Checklist

## Safety Gates (CRITICAL)
- [ ] **Gating Required**: `-enablelocalmining=1` flag required on daemon
- [ ] **Explicit Consent**: `i_understand:true` required in `mining.start` 
- [ ] **Network Detection**: Refuse mining on mainnet without explicit enable
- [ ] **Sync Check**: Refuse mining if not fully synced (IBD active)

## Difficulty & Target (PRODUCTION)
- [ ] **Dynamic Bits**: Use bits from GBT, not hardcoded powLimit
- [ ] **Network-Specific**: 
  - Regtest: Keep `0x207fffff` (easy)
  - Testnet: Use chain difficulty
  - Mainnet: Use chain difficulty (HARD)
- [ ] **Target Validation**: Verify PoW against actual network target

## Resource Limits (SAFETY)
- [ ] **Thread Caps**: `threads ≤ CPU-1` (never use all cores)
- [ ] **Throttle Range**: `throttle ∈ [0.15, 0.90]` (15-90%)
- [ ] **Memory Limits**: Cap work template cache size
- [ ] **CPU Priority**: Use nice/QoS lowered priority

## Thermal & Battery Protection (USER SAFETY)
- [ ] **Battery Detection**: Pause mining on laptop battery
- [ ] **Thermal Monitor**: Pause if CPU temperature > threshold
- [ ] **Auto-Resume**: Resume when conditions improve
- [ ] **Duty Cycle**: Default 35% throttle for thermal protection

## Implementation Snippets

### Safety Gates
```cpp
// In mining.start RPC handler
bool MiningSafetyGates::ValidateMainnetMining(const NetworkParams& params, 
                                             const MiningConfig& config) {
    // 1. Check explicit enable flag
    if (params.network == "mainnet" && !gArgs.GetBoolArg("-enablelocalmining", false)) {
        throw std::runtime_error("Local mining disabled. Use -enablelocalmining=1 to enable.");
    }
    
    // 2. Require explicit understanding
    if (params.network != "regtest" && !config.i_understand) {
        throw std::runtime_error("Mining requires i_understand:true for safety.");
    }
    
    // 3. Check sync status
    if (g_blockchain->getBlockHeight() < getBestKnownHeight() - 10) {
        throw std::runtime_error("Cannot mine while syncing. Wait for full sync.");
    }
    
    return true;
}
```

### Dynamic Difficulty
```cpp
// In GBT handler - use chain difficulty, not powLimit
uint32_t GetNetworkBits(const std::string& network) {
    if (network == "regtest") {
        return 0x207fffff; // Keep easy for development
    } else {
        // Use actual chain difficulty from tip
        auto tipHeader = g_blockchain->getTipHeader();
        return tipHeader.bits; // Real network difficulty
    }
}
```

### Resource Limits
```cpp
// In mining.start parameter validation
void ValidateMiningLimits(const MiningConfig& config) {
    int maxThreads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    if (config.threads > maxThreads) {
        throw std::runtime_error("Too many threads. Max: " + std::to_string(maxThreads));
    }
    
    if (config.throttle < 0.15 || config.throttle > 0.90) {
        throw std::runtime_error("Throttle must be between 0.15 and 0.90");
    }
}
```

### Thermal Protection
```cpp
// In MiningEngine::WorkerLoop
void MiningEngine::CheckThermalConditions() {
    #ifdef __APPLE__
    // macOS thermal check
    if (getThermalState() > THERMAL_STATE_FAIR) {
        PauseMining("thermal");
        return;
    }
    #endif
    
    // Battery check
    if (isOnBattery()) {
        PauseMining("battery");
        return;
    }
    
    // Resume if conditions good
    if (isPaused() && canResumeMining()) {
        ResumeMining();
    }
}
```

## Verification Commands
```bash
# Test mainnet gating (should fail without flag)
./dinerod -testnet  # Should refuse mining.start without -enablelocalmining=1

# Test resource limits
curl -d '{"method":"mining.start","params":[{"threads":999}]}' # Should fail

# Test throttle limits  
curl -d '{"method":"mining.start","params":[{"throttle":0.05}]}' # Should fail

# Test sync requirement
# Start daemon on testnet, try mining before sync complete # Should fail
```

## Production Deployment
1. **Default OFF**: Mining disabled by default
2. **Clear Warnings**: GUI shows "Mining uses significant CPU/battery"  
3. **Monitoring**: Expose thermal/battery pause metrics
4. **Documentation**: Clear setup guide for safe mining
5. **Support**: Clear troubleshooting for common issues

## 🚨 NEVER SHIP WITHOUT THESE SAFETY CHECKS 🚨
