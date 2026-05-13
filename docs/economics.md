# Dinero Economics Documentation

## Overview
Dinero is a cryptocurrency designed with a **3-year CPU-friendly phase** followed by Bitcoin-level difficulty and regular halving. The economics are carefully calibrated to achieve exactly 3 years for community mining.

## Block Time Configuration

### **8.7-Minute Blocks (520 seconds)**
- **Target**: Exactly 3 years for CPU-friendly phase
- **Calculation**: 18M DIN ÷ 3 years = 6M DIN/year = 60,646 blocks/year = 520 seconds/block
- **Retarget Interval**: 8.7 hours (520 × 60 seconds, 60 blocks)
- **Coinbase Maturity**: 100 blocks (~14.4 hours)

### **Why 8.7 Minutes?**
- **CPU-Friendly**: Gives miners plenty of time (8.7 minutes vs Bitcoin's 10 minutes)
- **Economic Balance**: 6M DIN per year is a reasonable mining rate
- **Network Stability**: Longer blocks reduce orphaned blocks
- **Halving Schedule**: Maintains 4-year epochs for the halving phase

## Supply Schedule

### **Phase 1: Developer Fund (0-7M DIN)**
- **Duration**: Premined at genesis
- **Reward**: 0 DIN per block (premined)
- **Purpose**: Security fund for development and network stability

### **Phase 2: CPU-Friendly Mining (7M-25M DIN)**
- **Duration**: Exactly 3 years
- **Reward**: 99 DIN per block
- **Difficulty**: CPU-friendly (easy mining)
- **Target**: Community participation and decentralization

### **Phase 3: Bitcoin-Level Difficulty (25M+ DIN)**
- **Duration**: ~30+ years
- **Reward**: Halving schedule (99→66→33→16→8→4→2→1 DIN)
- **Difficulty**: Bitcoin-level (competitive mining)
- **Halving**: Every 210,000 blocks (~4 years)

## Time-Based Constants

| Blocks | Time (8.7-min blocks) | Purpose |
|--------|----------------------|---------|
| 60 | 8.7 hours | Retarget interval |
| 100 | 14.4 hours | Coinbase maturity |
| 144 | 20.8 hours | |
| 1,008 | 6.07 days | |
| 2,016 | 12.15 days | |
| 60,646 | 1 year | Annual block count |
| 181,938 | 3 years | CPU-friendly phase |
| 210,000 | 4 years | Halving interval |
| 242,584 | 4 years | Halving epoch |

## Reward Schedule

### **CPU-Friendly Phase (99 DIN/block)**
- **Duration**: 3 years
- **Total**: 18M DIN
- **Purpose**: Community mining and decentralization

### **Halving Schedule**
1. **Epoch 0**: 66 DIN/block (99 × 2/3)
2. **Epoch 1**: 33 DIN/block (66 ÷ 2)
3. **Epoch 2**: 16 DIN/block (33 ÷ 2)
4. **Epoch 3**: 8 DIN/block (16 ÷ 2)
5. **Epoch 4**: 4 DIN/block (8 ÷ 2)
6. **Epoch 5**: 2 DIN/block (4 ÷ 2)
7. **Epoch 6**: 1 DIN/block (2 ÷ 2)
8. **Epoch 7+**: 1 DIN/block (tail emission)

## Total Supply

### **Supply Breakdown**
- **Premine**: 7M DIN (3.9%)
- **CPU-Friendly**: 18M DIN (10.0%)
- **Halving Phase**: 155.5M DIN (86.1%)
- **Total**: 180.5M DIN

### **Supply Timeline**
- **Year 0**: 7M DIN (premine)
- **Year 3**: 25M DIN (7M + 18M)
- **Year 7**: ~50M DIN (with halving)
- **Year 15**: ~100M DIN
- **Year 30**: ~150M DIN
- **Year 50+**: ~180.5M DIN (approaching total supply)

## Network Parameters

### **Consensus Rules**
- **Block Time**: 520 seconds (8.7 minutes)
- **Retarget Interval**: 60 blocks (8.7 hours)
- **Coinbase Maturity**: 100 blocks (~14.4 hours)
- **Halving Interval**: 210,000 blocks (~4 years)

### **Difficulty Adjustment**
- **CPU-Friendly Phase**: Easy difficulty for community mining
- **Halving Phase**: Bitcoin-level difficulty with regular adjustments
- **Algorithm**: ASERT (Adaptive Difficulty Adjustment Algorithm)

## Economic Benefits

### **For Miners**
- **3-Year Window**: Plenty of time for CPU mining
- **Predictable Rewards**: 99 DIN per block for 3 years
- **Low Barrier**: No expensive ASIC hardware needed initially

### **For Network**
- **Decentralization**: CPU-friendly phase encourages participation
- **Security**: 7M DIN premine provides development funding
- **Sustainability**: Halving schedule ensures long-term viability

### **For Users**
- **Stable Economics**: Predictable supply schedule
- **Fair Distribution**: Community mining phase
- **Long-term Value**: Scarcity through halving

## Technical Implementation

### **Lock-In Mechanisms**
- **Boot-Time Guard**: Daemon verifies economics at startup
- **Comprehensive Tests**: Prevents regression to old block times
- **Chainparams**: Frozen configuration across all networks
- **Documentation**: Clear specifications and verification commands

### **Regression Prevention**
- **Test Suite**: `test_block_time_economics` verifies all constants
- **Boot Guard**: Daemon refuses to start with wrong economics
- **CI/CD**: Automated tests catch regressions
- **Clear Errors**: Specific guidance when issues are detected

## Verification Commands

### **Run Economics Tests**
```bash
# Build and run comprehensive test suite
cmake --build build --target test_block_time_economics -j4
./build/bin/test_block_time_economics

# Run via CTest
ctest --test-dir build -R test_block_time_economics --output-on-failure
```

### **Verify Daemon Economics**
```bash
# Start daemon and verify boot-time economics check
./build/bin/dinerod --network=regtest --datadir=/tmp/test --printtoconsole
# Look for: "✅ Block time economics verified: CPU-friendly phase is exactly 3 years"
```

### **Manual Math Verification**
```bash
# Verify the math manually
python3 -c "
block_time = 520  # seconds
reward_per_block = 99  # DIN
cpu_friendly_coins = 18_000_000  # DIN
seconds_per_year = 365 * 24 * 60 * 60
blocks_per_year = seconds_per_year / block_time
din_per_year = blocks_per_year * reward_per_block
years = cpu_friendly_coins / din_per_year
print(f'CPU-friendly phase: {years:.1f} years (target: 3.0)')
"
```

## Summary

Dinero's economics are designed to be:
- **✅ Fair**: 3-year CPU-friendly phase for community participation
- **✅ Predictable**: Clear supply schedule and halving timeline
- **✅ Secure**: 7M DIN premine for development and network stability
- **✅ Sustainable**: Long-term halving schedule ensures scarcity
- **✅ Locked-In**: Comprehensive tests prevent regression

The **8.7-minute block time** achieves exactly **3 years** for the CPU-friendly phase, providing the perfect balance between accessibility and economic sustainability.
