# 📅 DineroCoin Halving Schedule

**Date:** October 6, 2025  
**Status:** ✅ **DETERMINISTIC - Exact heights known in advance**

---

## 🎯 Complete Halving Timeline

### Phase 1: CPU-Friendly (No Halvings)

| Height Range | Reward | Duration (Blocks) | Duration (Years)* |
|-------------|--------|-------------------|-------------------|
| 2 - 180,001 | **100 DIN** | 180,000 | **~3.4 years** |

*Assuming 10 min/block average

---

## 🔄 Phase 2: Halving Epoch Schedule

### First Halving Period (50 DIN → 25 DIN)

| Attribute | Value |
|-----------|-------|
| **Start Height** | **180,002** |
| **End Height** | **979,999** |
| **Blocks** | 800,000 |
| **Reward** | **50 DIN per block** |
| **Total Mined** | 40,000,000 DIN |
| **Duration** | **~15.2 years** |
| **Start Date** | ~March 2029 |
| **HALVING DATE** | **~June 2044** |

---

### Second Halving Period (25 DIN → 12.5 DIN)

| Attribute | Value |
|-----------|-------|
| **Start Height** | **980,002** |
| **End Height** | **1,779,999** |
| **Blocks** | 800,000 |
| **Reward** | **25 DIN per block** |
| **Total Mined** | 20,000,000 DIN |
| **Duration** | **~15.2 years** |
| **Start Date** | ~June 2044 |
| **HALVING DATE** | **~September 2059** |

---

### Third Halving Period (12.5 DIN → 6.25 DIN)

| Attribute | Value |
|-----------|-------|
| **Start Height** | **1,780,002** |
| **End Height** | **2,579,999** |
| **Blocks** | 800,000 |
| **Reward** | **12.5 DIN per block** |
| **Total Mined** | 10,000,000 DIN |
| **Duration** | **~15.2 years** |
| **Start Date** | ~September 2059 |
| **HALVING DATE** | **~December 2074** |

---

## 📊 Complete Halving Schedule (First 10 Halvings)

| Halving # | Start Height | Reward (DIN) | Duration | Estimated Date | Total Mined |
|-----------|-------------|--------------|----------|----------------|-------------|
| **0** (First) | 180,002 | **50** → 25 | 800K blocks (~15.2 years) | March 2029 → **June 2044** | 40M DIN |
| **1** | 980,002 | **25** → 12.5 | 800K blocks (~15.2 years) | June 2044 → **Sept 2059** | 20M DIN |
| **2** | 1,780,002 | **12.5** → 6.25 | 800K blocks (~15.2 years) | Sept 2059 → **Dec 2074** | 10M DIN |
| **3** | 2,580,002 | **6.25** → 3.125 | 800K blocks (~15.2 years) | Dec 2074 → **March 2090** | 5M DIN |
| **4** | 3,380,002 | **3.125** → 1.5625 | 800K blocks (~15.2 years) | March 2090 → **June 2105** | 2.5M DIN |
| **5** | 4,180,002 | **1.5625** → 0.78125 | 800K blocks (~15.2 years) | June 2105 → **Sept 2120** | 1.25M DIN |
| **6** | 4,980,002 | **0.78125** → 0.390625 | 800K blocks (~15.2 years) | Sept 2120 → **Dec 2135** | 625K DIN |
| **7** | 5,780,002 | **0.390625** → 0.195313 | 800K blocks (~15.2 years) | Dec 2135 → **March 2151** | 312.5K DIN |
| **8** | 6,580,002 | **0.195313** → 0.097656 | 800K blocks (~15.2 years) | March 2151 → **June 2166** | 156.25K DIN |
| **9** | 7,380,002 | **0.097656** → 0.048828 | 800K blocks (~15.2 years) | June 2166 → **Sept 2181** | 78.125K DIN |

---

## 🧮 Math Behind the Schedule

### Halving Interval:
```
HALVING_INTERVAL = 800,000 blocks

Duration per halving:
800,000 blocks × 10 minutes/block = 8,000,000 minutes
                                   = 133,333 hours
                                   = 5,555 days
                                   = ~15.2 years
```

### Reward Calculation:
```cpp
uint64_t CalculateHalvingReward(uint32_t halving_epoch) {
    uint64_t reward = PHASE2_INITIAL_REWARD; // 50 DIN
    
    for (uint32_t i = 0; i < halving_epoch; i++) {
        reward /= 2;  // Halve each epoch
        
        if (reward < UNA_PER_DIN) { // Min 1 DIN
            reward = UNA_PER_DIN;
            break;
        }
    }
    
    return reward;
}
```

### Height Calculation:
```cpp
// First halving block
uint32_t first_halving = PHASE1_START_HEIGHT + PHASE1_BLOCKS + HALVING_INTERVAL;
// = 2 + 180,000 + 800,000
// = 980,002

// Second halving block
uint32_t second_halving = first_halving + HALVING_INTERVAL;
// = 980,002 + 800,000
// = 1,780,002

// Nth halving block
uint32_t nth_halving = PHASE1_START_HEIGHT + PHASE1_BLOCKS + (N * HALVING_INTERVAL);
```

---

## 📈 Supply Curve

```
Total Supply Over Time:

Genesis + Premine: 1M DIN (instant)
Phase 1 (3.4 years): +18M DIN → 19M DIN total
Halving 0 (15.2 years): +40M DIN → 59M DIN total
Halving 1 (15.2 years): +20M DIN → 79M DIN total
Halving 2 (15.2 years): +10M DIN → 89M DIN total
Halving 3 (15.2 years): +5M DIN → 94M DIN total
Halving 4+ (decades): +3.85M DIN → 97.85M DIN total (cap reached)
```

---

## 🎯 Key Milestones

### Short Term (0-5 years):
- **October 2025:** Genesis launch
- **March 2029:** CPU-friendly phase ends, first halving period begins (50 DIN)

### Medium Term (5-20 years):
- **June 2044:** **FIRST HALVING** (50 → 25 DIN) ⭐
- **September 2059:** Second halving (25 → 12.5 DIN)

### Long Term (20+ years):
- **December 2074:** Third halving (12.5 → 6.25 DIN)
- **March 2090:** Fourth halving (6.25 → 3.125 DIN)
- **2100+:** Reward drops below 1 DIN, tail emission begins

---

## ⏱️ Comparison with Bitcoin

| Attribute | Bitcoin | DineroCoin |
|-----------|---------|------------|
| **Initial Reward** | 50 BTC | 50 DIN (Phase 2) |
| **Halving Interval** | 210,000 blocks | 800,000 blocks |
| **Halving Duration** | ~4 years | ~15.2 years |
| **Total Halvings** | ~64 | ~8-10 (until tail emission) |
| **Block Time** | ~10 minutes | ~10 minutes |
| **Max Supply** | 21M BTC | 97.85M DIN |
| **Tail Emission** | No | Yes (1 DIN minimum) |

**Key Difference:** DineroCoin has LONGER halving periods (3.8x longer) for more stable supply.

---

## 🔮 Monitoring Next Halving

### RPC Command:
```bash
curl http://127.0.0.1:20998/ --cookie ./data/.cookie \
  -d '{"method":"getblockchaininfo"}' | jq .

# Response includes:
{
  "result": {
    "blocks": 500000,
    "phase": "cpu_friendly" | "halving",
    "next_block_reward": 100 | 50 | 25 | ...,
    "blocks_until_halving": 480002,
    "estimated_halving_date": "2044-06-15"
  }
}
```

---

## 🚨 Important Notes

1. **First Halving is FAR AWAY:** ~19 years from genesis (June 2044)
2. **Plenty of Time:** Miners will have 15+ years at 50 DIN reward
3. **Predictable:** All heights known in advance, no surprises
4. **Stable Supply:** Longer halving periods = smoother economics

---

## 📊 Visual Timeline

```
2025: Genesis (1M premine)
      |
      |--- Phase 1: CPU-friendly (3.4 years)
      |    100 DIN/block
      |
2029: Phase 2 begins
      |
      |--- Halving 0 (15.2 years)
      |    50 DIN/block
      |
2044: ⭐ FIRST HALVING ⭐
      |
      |--- Halving 1 (15.2 years)
      |    25 DIN/block
      |
2059: Second Halving
      |
      |--- Halving 2 (15.2 years)
      |    12.5 DIN/block
      |
2074: Third Halving
      ...
```

---

## 🎯 Answer: When is 50 → 25 Halving?

**Height:** **980,002**  
**Date:** **~June 2044**  
**Time from Genesis:** **~19 years**  
**Time from Phase 2 Start:** **~15.2 years**

**This is intentionally long to provide economic stability!** 🚀

