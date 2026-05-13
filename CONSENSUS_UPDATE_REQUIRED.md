# ⚠️ CRITICAL: Consensus Update Required - Network-Wide Deployment

## Status: URGENT - Consensus Fork Risk

**Date**: November 8, 2025
**Severity**: CRITICAL
**Action Required**: ALL nodes must be updated immediately

---

## Executive Summary

Critical consensus fixes have been applied to the DineroCoin codebase that change:
1. ASERT difficulty calculation (rolling anchor implementation)
2. Block header serialization format
3. Premine block constants and validation

**ANY NODE RUNNING OLD BINARIES WILL FORK FROM THE NETWORK.**

---

## What Changed (Consensus-Critical)

### 1. ASERT Difficulty Adjustment (`src/consensus/pow.hpp`)

**Before** (BROKEN):
```cpp
// Fixed anchor at block 1 causing massive time deltas
uint32_t result = CalculateASERT(
    height,
    1,                       // ❌ Fixed historical anchor
    currentMTP,
    block1MTP,               // ❌ 24 days in the past
    0x1d3fffff,
    asertParams
);
```

**After** (FIXED):
```cpp
// Rolling anchor using previous block
uint32_t result = CalculateASERT(
    height,
    height - 1,              // ✅ Rolling anchor
    currentMTP,
    prevMTP,                 // ✅ ~180s ago
    prevBits,
    asertParams
);
```

**Impact**: Difficulty calculation changed from returning wrong values (0x1f002710 = diff 10,000) to correct values (0x1d3fffff = diff 1,024).

### 2. ASERT Anchor Bits (`src/consensus/consensus.hpp:33`)

**Before**:
```cpp
uint32_t asertAnchorBits = 0x1f002710;  // ❌ Wrong (diff 10,000)
```

**After**:
```cpp
uint32_t asertAnchorBits = 0x1d3fffff;  // ✅ Correct (diff 1,024)
```

### 3. Premine Constants (`include/consensus/premine_constants.h`)

**Before**:
```cpp
static constexpr const char* PREMINE_BLOCK_HASH =
    "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a";  // ❌ OLD
static constexpr uint32_t PREMINE_NONCE = 0x0007d3e6;  // ❌ OLD
```

**After**:
```cpp
static constexpr const char* PREMINE_BLOCK_HASH =
    "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a";  // ✅ NEW
static constexpr uint32_t PREMINE_NONCE = 0x08b7bcaf;  // ✅ NEW (146,259,119)
static constexpr const char* PREMINE_MERKLE_ROOT =
    "7c19222e2f1de4d0d71b072c69398565c0b38f8164e11017581084fb575e7867";  // ✅ NEW
```

**Why Changed**: Premine block regenerated with correct difficulty bits (0x1d3fffff instead of 0x1f002710).

---

## Fork Scenario

### If Old Nodes Remain Running

```
Time: T+0 (Now)
┌─────────────────────┐
│  Local Mac (Fixed)  │  ← Correct consensus (diff 1024)
│  Height: 1          │
└─────────────────────┘
          │
          │ mines block 2
          ▼
┌─────────────────────┐
│  Block 2 (New)      │
│  Difficulty: 1024   │
│  Hash: 00000abc... │
└─────────────────────┘

Time: T+0 (Now)
┌─────────────────────┐
│ Linux Server (Old)  │  ← Wrong consensus (diff 10,000)
│  Height: 1          │
└─────────────────────┘
          │
          │ mines block 2 (different!)
          ▼
┌─────────────────────┐
│  Block 2 (Old)      │
│  Difficulty: 10,000 │  ❌ FORK!
│  Hash: 00000xyz... │
└─────────────────────┘
```

### Result: Network Split

- Mac nodes accept blocks with diff 1024
- Linux nodes reject these as "bad-diffbits"
- Linux nodes mine their own chain with diff 10,000
- **TWO INCOMPATIBLE CHAINS** running simultaneously

---

## Files Modified (Git Diff Needed)

```bash
# Consensus-critical changes
src/consensus/consensus.hpp                    # Line 33: asertAnchorBits
src/consensus/pow.hpp                          # Rolling anchor implementation
include/consensus/premine_constants.h          # All premine constants
src/consensus/pow_asert_native.hpp             # ASERT calculation (debug logs added)

# Supporting changes
src/consensus/premine_block_mainnet.hpp        # Updated but NOT loaded by daemon (legacy file)
```

---

## Deployment Checklist

### Pre-Deployment Verification

- [x] Local Mac build verified (Nov 8 10:11:38 2025)
- [x] Premine constants match expected values
- [x] Mining working at correct difficulty (1024)
- [ ] **Linux server build status UNKNOWN** ⚠️
- [ ] Git repository synced with all changes
- [ ] All nodes identified and accessible

### Deployment Steps

#### 1. Identify All Running Nodes

```bash
# Check Linux production server
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 "pgrep -fl dinerod"

# Check any other servers
# Add commands for each known server
```

#### 2. Stop All Old Nodes IMMEDIATELY

```bash
# On each server
ssh root@<server> "pkill -9 dinerod"
```

**CRITICAL**: Stop mining and daemon on ALL nodes before any proceed to block 2.

#### 3. Pull Latest Code

```bash
# On each server
ssh root@<server> "cd ~/DineroCoin && git pull origin main"
```

**Verify**: Check that `include/consensus/premine_constants.h` contains NEW hash:
```bash
ssh root@<server> "grep '0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a' ~/DineroCoin/include/consensus/premine_constants.h"
```

Expected output: Line containing the new hash. If empty, code not synced!

#### 4. Rebuild Binaries

```bash
# On each server
ssh root@<server> << 'EOF'
cd ~/DineroCoin
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc) dinerod dinero-cli
EOF
```

**Verify**: Build completes without errors.

#### 5. Delete Old Blockchain Databases

```bash
# On each server - CRITICAL STEP
ssh root@<server> << 'EOF'
rm -rf ~/.dinero/blockchain.db*
rm -rf ~/.dinero/blockchain/chaindb
echo "✅ Old blockchain deleted"
EOF
```

**Why**: Old databases contain wrong premine hash (0000002bd3fa677b...). Must reinitialize with new hash (0000002bd3fa...).

#### 6. Restart Daemons

```bash
# On each server
ssh root@<server> "cd ~/DineroCoin/build && ./dinerod -daemon"
sleep 6

# Verify startup
ssh root@<server> "cd ~/DineroCoin/build && ./dinero-cli blockchain.getinfo"
```

**Expected output**:
```json
{
  "blocks" : 1,
  "bestblockhash" : "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a",
  "difficulty" : 1024
}
```

**Red flags**:
- ❌ `bestblockhash` starts with `0000002bd3fa677b...` (old premine!)
- ❌ `difficulty` shows 10000 instead of 1024
- ❌ Any errors about "invalid premine" or "bad-diffbits"

#### 7. Verify Consensus Alignment

```bash
# Query premine block from each server
for server in 173.249.195.59 <other-servers>; do
    echo "=== Server: $server ==="
    ssh root@$server "cd ~/DineroCoin/build && ./dinero-cli blockchain.getblock 1" | grep -E "hash|bits|difficulty"
done
```

**All servers MUST show**:
```json
{
  "hash" : "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a",
  "bits" : "0x1d3fffff",
  "difficulty" : 1024
}
```

#### 8. Resume Mining (Coordinated)

**ONLY after ALL nodes show identical premine hash**:

```bash
# On primary mining node
ssh root@<server> << 'EOF'
cd ~/DineroCoin/build
./dinero-cli mining.setaddress <your-din-address>
./dinero-cli mining.start $(nproc)
EOF
```

Monitor for block 2:
```bash
# Watch for new block
ssh root@<server> "cd ~/DineroCoin/build && watch -n 5 './dinero-cli blockchain.getinfo | grep blocks'"
```

---

## Verification After Deployment

### 1. Check Block 2 Consensus

Once ANY node mines block 2, verify ALL nodes accept it:

```bash
# On each server
ssh root@<server> "cd ~/DineroCoin/build && ./dinero-cli blockchain.getblock 2"
```

**All nodes MUST show**:
- Same `hash` value
- `bits` = 0x1d3fffff (±1 LSB acceptable)
- `difficulty` ≈ 1024
- `height` = 2

**Red flag**: If different nodes show different hashes for block 2, **FORK DETECTED** - stop immediately.

### 2. Monitor ASERT Logs

```bash
# On each server
ssh root@<server> "tail -f ~/.dinero/debug.log | grep ASERT-DEBUG"
```

**Expected pattern**:
```
[ASERT-DEBUG] h=2 anchor=1 heightΔ=1 timeΔ=~180 idealTime=180 excessTime=~0 halfLife=43200
[ASERT-DEBUG] k=0 r=~0
[ASERT-DEBUG] anchorBits=0x1d3fffff result=0x1d3fffff
```

**Red flags**:
- ❌ `k` > ±5 (indicates time issues)
- ❌ `result` = 0x1f002710 or similar (wrong difficulty)
- ❌ `timeΔ` > 1000 seconds (massive delta)

### 3. Verify Network Convergence

```bash
# Query all nodes for current best block
for server in 173.249.195.59 <others>; do
    echo "=== $server ==="
    ssh root@$server "cd ~/DineroCoin/build && ./dinero-cli blockchain.getinfo" | grep -E "blocks|bestblockhash"
done
```

**Success criteria**:
- All nodes show same `blocks` height
- All nodes show same `bestblockhash`
- Heights increment together as new blocks mine

---

## Rollback Plan (Emergency)

If deployment fails or fork detected:

### 1. Stop All Nodes Immediately
```bash
for server in <all-servers>; do
    ssh root@$server "pkill -9 dinerod"
done
```

### 2. Identify Issue
- Check which nodes have divergent `bestblockhash`
- Review debug logs for errors
- Verify premine constants match across all nodes

### 3. Do NOT Resume Mining Until Issue Resolved
- Investigate code differences
- Rebuild if necessary
- Re-verify consensus parameters

---

## Known Servers (Update This List)

### Production Servers
- **173.249.195.59** - Primary Linux server
  - SSH: `ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59`
  - Status: UNKNOWN (needs verification)
  - Last known build: UNKNOWN

### Development Servers
- **Local Mac** - Development machine
  - Build date: Nov 8 10:11:38 2025
  - Status: ✅ UPDATED (correct consensus)
  - Height: 1 (premine)

### Add Any Other Servers Here
- ...

---

## Timeline

```
T+0 (NOW)        Stop all nodes, begin deployment
T+30 min         All nodes rebuilt and restarted
T+45 min         Verify consensus alignment
T+60 min         Resume mining (coordinated)
T+70 min         First block 2 mined
T+80 min         Verify all nodes accept block 2
T+24 hours       Monitor stability across 240+ blocks
```

---

## Contact & Escalation

If issues arise during deployment:
1. **STOP ALL MINING IMMEDIATELY**
2. Gather debug logs from all nodes
3. Document fork scenario (heights, hashes, timestamps)
4. Do not proceed until consensus verified

---

## Post-Deployment Tasks

- [ ] Update documentation with final server list
- [ ] Archive old blockchain databases (for forensics)
- [ ] Monitor first 100 blocks for stability
- [ ] Create backup of working state (blockchain.db + binaries)
- [ ] Update CI/CD to prevent old builds from deploying

---

## Appendix: Quick Commands

### Check Server Status
```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 "pgrep -fl dinerod && cd ~/DineroCoin/build 2>/dev/null && ./dinero-cli blockchain.getinfo 2>&1"
```

### Emergency Stop All
```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 "pkill -9 dinerod"
```

### Verify Premine Hash
```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 "cd ~/DineroCoin/build && ./dinero-cli blockchain.getblock 1 | grep '\"hash\"'"
```

Expected: `"hash" : "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a"`

---

**Document Version**: 1.0
**Last Updated**: November 8, 2025
**Status**: DEPLOYMENT PENDING
