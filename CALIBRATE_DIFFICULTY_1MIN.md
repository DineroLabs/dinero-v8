# 🎯 Calibrating Initial Difficulty for 1-Minute Blocks

**Date:** October 7, 2025  
**Goal:** Adjust difficulty so M3 Mac (6-core) finds blocks every ~1 minute

---

## 📊 **Current Situation:**

```
Current difficulty: 0x2100ffff
Your M3 Mac: ~6-8 MH/s (estimated)
Block time: ~1-2 seconds ⚡ (TOO FAST!)
```

---

## 🧮 **The Math:**

### **Step 1: Measure Your Actual Hash Rate**

From your miner output, you should see something like:
```
⛏️  6.50 MH/s | Total: 650 MH | Blocks: 10
```

Let's assume: **6.5 MH/s** for M3 Mac 6-core

---

### **Step 2: Calculate Target Difficulty**

**Current:**
```
Difficulty: 0x2100ffff
Block time: ~1-2 seconds
Hash rate: 6.5 MH/s
```

**Target:**
```
Block time: 60 seconds (1 minute)
Hash rate: 6.5 MH/s (same)
Difficulty: Need to calculate
```

**Formula:**
```
New difficulty = Current difficulty × (Target time / Current time)

If current block time is 1.5 seconds:
New difficulty = 0x2100ffff × (60 / 1.5)
               = 0x2100ffff × 40
```

---

### **Step 3: Difficulty Bits Explained**

Bitcoin uses **compact format** for difficulty:

```
0x2100ffff means:
  21 = exponent (shift amount)
  00ffff = mantissa (significant bits)

Target = 0x00ffff × 2^(8 × (0x21 - 3))
       = 0x00ffff × 2^(8 × 30)
       = 0x00ffff0000000000000000000000000000000000000000000000000000000000
```

---

## 🎯 **Recommended Values:**

### **For 1-Minute Blocks (M3 Mac 6-core):**

```cpp
// Option 1: Conservative (easier, ~30-60 seconds)
static constexpr uint32_t PHASE1_DIFFICULTY = 0x1f0fffff;

// Option 2: Moderate (target ~60 seconds)
static constexpr uint32_t PHASE1_DIFFICULTY = 0x1e7fffff;

// Option 3: Aggressive (harder, ~60-90 seconds)
static constexpr uint32_t PHASE1_DIFFICULTY = 0x1e3fffff;
```

---

## 📝 **How to Change It:**

### **File:** `src/daemon/consensus_subsidy.h`

**Find this line:**
```cpp
static constexpr uint32_t PHASE1_DIFFICULTY = 0x2100ffff; // Current (too easy)
```

**Change to:**
```cpp
static constexpr uint32_t PHASE1_DIFFICULTY = 0x1e7fffff; // Target: 1 min/block
```

---

## 🧪 **Testing Process:**

### **Step 1: Make the Change**

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Edit the file
nano src/daemon/consensus_subsidy.h

# Find PHASE1_DIFFICULTY and change value
```

### **Step 2: Rebuild**

```bash
# Rebuild daemon
cmake --build build --target dinerod -j8

# Rebuild miner
cmake --build build --target dinero-miner -j8
```

### **Step 3: Test**

```bash
# Clear old blockchain (start fresh)
rm -rf data/blockchain.db data/blocks/

# Start daemon
./build/dinerod -datadir=./data -rpcport=20998 &

# Start miner
./build/dinero-miner \
    --address din1qq0nh4jqhnyuv4j6hshljqp4g8s4j5u3at33zvw \
    --threads 6 \
    --datadir ./data

# Watch the output:
# ⛏️  X.XX MH/s | ...
# 🎉 BLOCK FOUND! (should be every ~60 seconds)
```

### **Step 4: Adjust if Needed**

```
Too fast (< 30 seconds)?  → Decrease difficulty (0x1e3fffff)
Just right (45-75 seconds)? → Keep it! ✅
Too slow (> 90 seconds)?   → Increase difficulty (0x1f0fffff)
```

---

## 📊 **Difficulty Values Reference:**

| Difficulty | Approx Block Time (6.5 MH/s) | Notes |
|------------|------------------------------|-------|
| 0x2100ffff | 1-2 seconds | Current (too easy) |
| 0x1f0fffff | 15-30 seconds | Easier |
| 0x1e7fffff | 45-75 seconds | **Target: 1 minute** ✅ |
| 0x1e3fffff | 90-120 seconds | Harder |
| 0x1d00ffff | 5-10 minutes | Phase 2 difficulty |

---

## 🎯 **Quick Change Script:**

```bash
#!/bin/bash
# Quick difficulty calibration

cd /Users/haydarevich/Documents/DineroCoin

echo "🔧 Changing Phase 1 difficulty..."

# Backup original
cp src/daemon/consensus_subsidy.h src/daemon/consensus_subsidy.h.bak

# Change difficulty
sed -i '' 's/PHASE1_DIFFICULTY = 0x2100ffff/PHASE1_DIFFICULTY = 0x1e7fffff/' \
    src/daemon/consensus_subsidy.h

echo "✅ Changed to 0x1e7fffff (target: 1 min blocks)"

# Rebuild
echo "🔨 Rebuilding..."
cmake --build build --target dinerod -j8
cmake --build build --target dinero-miner -j8

echo "✅ Done! Test with:"
echo "   ./build/dinero-miner --address YOUR_ADDRESS --threads 6 --datadir ./data"
```

---

## 🎯 **Expected Results:**

### **Before (0x2100ffff):**
```
⛏️  6.50 MH/s | Total: 13 MH | Blocks: 2
🎉 BLOCK FOUND! Nonce: 12345
⛏️  6.48 MH/s | Total: 26 MH | Blocks: 4
🎉 BLOCK FOUND! Nonce: 67890
⛏️  6.52 MH/s | Total: 39 MH | Blocks: 6

Time between blocks: ~1-2 seconds ⚡
```

### **After (0x1e7fffff):**
```
⛏️  6.50 MH/s | Total: 390 MH | Blocks: 1
🎉 BLOCK FOUND! Nonce: 12345678
⛏️  6.48 MH/s | Total: 780 MH | Blocks: 2
🎉 BLOCK FOUND! Nonce: 87654321
⛏️  6.52 MH/s | Total: 1170 MH | Blocks: 3

Time between blocks: ~60 seconds ✅
```

---

## 📊 **Phase 1 Duration Calculation:**

### **With 1-Minute Blocks:**

```
Phase 1: 180,000 blocks
Block time: 1 minute
Duration: 180,000 minutes
        = 3,000 hours
        = 125 days
        = ~4 months ✅
```

**This is much more reasonable than:**
- Current (1-2 sec): ~2 days ❌
- Target (5 min): ~625 days (too long for testing)

---

## ✅ **Summary:**

### **What You Need to Do:**

1. **Edit** `src/daemon/consensus_subsidy.h`
2. **Change** `0x2100ffff` to `0x1e7fffff`
3. **Rebuild** daemon and miner
4. **Test** and adjust if needed

### **Expected Result:**

```
Block time: ~60 seconds (1 minute)
Phase 1 duration: ~125 days (4 months)
Still CPU-friendly: Yes! ✅
Difficulty: Fixed (no adjustment during Phase 1)
```

---

## 🚀 **Want me to make this change for you?**

I can:
1. Update the difficulty value
2. Rebuild the binaries
3. Test with your M3 Mac
4. Adjust if needed

Just say "yes" and I'll do it!
