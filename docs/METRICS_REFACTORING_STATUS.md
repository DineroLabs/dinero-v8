# Metrics Refactoring - Current State

## ✅ **What's Done**

### **1. MetricsRegistry API Updated** ✅
- Added `LabelMap` type (`std::map<std::string, std::string>`)
- All mining metric methods now accept optional `LabelMap` parameter
- Export functions (`ExportMetrics()`, `ExportMetricsJSON()`) support labels
- Backward compatible (empty labels = unlabeled metrics)

**Files**:
- `include/metrics/metrics_registry.h` - API updated
- `src/metrics/metrics_registry.cpp` - Implementation updated

### **2. MiningService Uses Labels** ✅
- `MiningService::UpdateTelemetry()` uses labels with `miner_id`
- Creates `LabelMap` with `{{"miner_id", miner_id_}}`
- Calls: `SetMiningHashrate()`, `SetMiningThreads()`, `SetMiningUptime()` with labels

**File**: `src/daemon/services/mining_service.cpp` (lines 259-269)

---

## ❌ **What's NOT Done**

### **1. mining_engine.cpp Still Uses Old API** ❌

**File**: `src/daemon/mining_engine.cpp`

**Call Sites WITHOUT Labels** (10 occurrences):

1. **Line 49**: `SetMiningThreads(config.numThreads)` - no labels
2. **Line 50**: `SetMiningUptime(0.0)` - no labels  
3. **Line 175**: `SetMiningJobHeight(template_.height)` - no labels
4. **Line 176**: `SetMiningCurrentBits(template_.bits)` - no labels
5. **Line 309**: `IncrementMiningBlocksFound()` - no labels
6. **Line 378**: `SetMiningHashrate(m_stats.hashrateHps.load())` - no labels
7. **Line 379**: `SetMiningThreads(static_cast<int>(m_currentThreads.load()))` - no labels
8. **Line 384**: `SetMiningUptime(uptime)` - no labels
9. **Line 438**: `SetMiningHashrate(hashrate)` - no labels
10. **Line 439**: `SetMiningUptime(static_cast<double>(uptime))` - no labels

### **2. Missing miner_id in MiningEngine** ❌

**Problem**: `MiningEngine` class doesn't have a `miner_id` member
- Cannot pass `miner_id` labels to MetricsRegistry
- All metrics from `mining_engine.cpp` appear as unlabeled

**Solution Needed**:
- Add `miner_id` member to `MiningEngine` class
- Initialize it in constructor or `Start()` method
- Pass labels to all MetricsRegistry calls

---

## 📊 **Current Behavior**

### **Metrics from MiningService**:
```
din_mining_hashrate_hps{miner_id="miner_1234567890"} 1234.56
din_mining_threads{miner_id="miner_1234567890"} 4
din_mining_uptime_seconds{miner_id="miner_1234567890"} 3600
```

### **Metrics from mining_engine.cpp**:
```
din_mining_hashrate_hps 1234.56  # No labels!
din_mining_threads 4              # No labels!
din_mining_uptime_seconds 3600    # No labels!
din_mining_blocks_found_total 1   # No labels!
```

**Result**: Mixed metrics - some labeled, some unlabeled. Cannot track per-miner stats properly.

---

## 🎯 **What Needs to Be Done**

### **1. Add miner_id to MiningEngine** 
- Add `std::string miner_id_` member
- Initialize in constructor or `Start()` method
- Generate unique ID (similar to MiningService)

### **2. Update All Call Sites in mining_engine.cpp**
- Replace all 10 MetricsRegistry calls to include labels
- Use `{{"miner_id", miner_id_}}` for all mining metrics

### **3. Verify Export**
- Check `/metrics` endpoint shows labeled metrics
- Verify Prometheus format includes `miner_id` labels
- Verify JSON format includes labels

---

## 📋 **Summary**

| Component | Status | Labels Used? |
|-----------|--------|--------------|
| MetricsRegistry API | ✅ Done | N/A (API ready) |
| MiningService | ✅ Done | ✅ Yes (`miner_id`) |
| mining_engine.cpp | ❌ Not Done | ❌ No (10 call sites) |
| MiningEngine class | ❌ Not Done | ❌ No `miner_id` member |

**Overall**: ~50% complete - API ready, but implementation incomplete.

