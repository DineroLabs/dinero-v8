# DineroCoin: Post-M.0 Roadmap
**Status as of December 19, 2025**

---

## ✅ COMPLETED & LOCKED

### Phase M.0: uint256 Integrity ✅
**Status:** 🔒 **LOCKED FOREVER**
- Identity layer uses uint256 throughout
- .GetHex() only at presentation boundaries
- Zero violations in consensus/daemon layers
- Enforcement system installed (one-liner protection)

**Files Protected:**
- `src/consensus/*` - 100% uint256 purity
- `src/daemon/*` - 100% uint256 purity
- Automated enforcement via pre-commit + CI

**Achievement:**
```bash
grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" \
  src/consensus src/daemon && exit 1
# Result: ✅ CLEAN (0 violations)
```

---

## 🎯 CRITICAL FOUNDATION (Already Have)

### ✅ You Should Not Touch These Again:

| Area | Status | Notes |
|------|--------|-------|
| **Identity (uint256)** | ✅ Locked | Phase M.0 complete + enforced |
| **UTXO set + undo** | ✅ Implemented | Block undo exists |
| **Block validation** | ✅ Exists | Transaction validator in place |
| **Block storage (RocksDB)** | ✅ Exists | ChainDB operational |
| **Wallet basic ops** | ✅ Exists | HD wallet functional |
| **Mining block template** | ✅ Exists | BlockAssembler ready |
| **Header-first groundwork** | ✅ Exists | HeaderSyncManager present |

**This is huge. Most projects never get here.**

---

## 🔴 CRITICAL: What Still Must Be Fixed

### Priority Order (Low Risk, High Impact)

---

### 1️⃣ **Header Sync Must Fully Drive Block Download**
**Phase:** H completion
**Priority:** 🔴 **CRITICAL**

**Current State:**
- ✅ Headers exist
- ✅ HeaderSyncManager exists
- ❌ Headers do not fully orchestrate block fetch + validation

**Required:**
```
Deterministic flow:
headers → best chain selection → block download → validation → connect
```

**Checklist:**
- [ ] Header chainwork comparison is final authority
- [ ] Orphan blocks queued until parents exist
- [ ] Block requests driven by headers, not peers randomly
- [ ] BlockIndex populated before validation (lightweight)

**Risk if skipped:**
⚠️ Reorgs and long forks will break you later

**Files to check:**
- `src/consensus/header_sync_manager.cpp`
- `src/consensus/chain_manager.cpp`
- `src/consensus/orphan_manager.cpp`

---

### 2️⃣ **ActivateBestChain (or Equivalent)**
**Phase:** Core orchestration
**Priority:** 🔴 **MANDATORY**

**Bitcoin Core's single most important function.**

**You must have:**
A function that:
1. Picks best chain by chainwork
2. Disconnects old blocks
3. Connects new blocks
4. Updates tip atomically

**Checklist:**
- [ ] Implement ActivateBestChain() or equivalent exists
- [ ] Undo data is applied correctly on disconnect
- [ ] Mempool is updated on reorg
- [ ] UTXO set is rolled back/forward correctly
- [ ] Tip update is atomic (no partial states)

**Risk if missing:**
❌ Node will appear to work but silently corrupt state

**Files to audit:**
- `src/consensus/chain_manager.cpp`
- Look for: `ActivateBestChain`, `ConnectTip`, `DisconnectTip`

---

### 3️⃣ **Mempool ↔ Block ↔ Mempool Loop Closure**
**Phase:** M.1 core subset
**Priority:** 🔴 **CRITICAL**

**Forget fee policies and RBF for now. You only need this minimal loop:**

**Required (minimal):**
- [ ] Accept valid tx into mempool
- [ ] Prevent double-spends via UTXO view
- [ ] Remove txs when block is connected
- [ ] Re-add txs on block disconnect (reorg safety)

**You can ignore (for now):**
- RBF
- CPFP
- Ancestor limits
- Fee estimation accuracy

**This is required for:**
- Mining safety
- Reorg correctness

**Files to check:**
- `src/daemon/mempool.cpp`
- `src/consensus/chain_manager.cpp` (block connect/disconnect hooks)

---

### 4️⃣ **Persistence on Restart**
**Phase:** Restart safety
**Priority:** 🔴 **CRITICAL** (Often forgotten, always fatal)

**After restart, verify:**
- [ ] ChainDB restores tip correctly
- [ ] UTXO set restores correctly
- [ ] BlockIndex reconnects
- [ ] Mempool is either:
  - Safely rebuilt, OR
  - Fully dropped (acceptable for now)

**Minimal acceptable behavior:**
```
Restart → node syncs → continues normally
```

**Test script:**
```bash
# Start node
./dinero-daemon

# Mine blocks, create transactions
# Stop node (SIGTERM)

# Restart
./dinero-daemon

# Verify:
# - Same tip height
# - Same best block hash
# - UTXO set intact
```

**Files to audit:**
- `src/daemon/services/chainstate_service.cpp` (startup/shutdown)
- `src/storage/chain_db.cpp` (persistence)

---

### 5️⃣ **P2P Block Flow (Minimal)**
**Phase:** P2P core
**Priority:** 🟡 **IMPORTANT** (not fancy networking)

**You do NOT need:**
- Peer scoring
- Rate limiting
- Fancy inv logic

**You DO need:**
- [ ] `inv` → `getdata` → `block` flow
- [ ] Ignore duplicate blocks
- [ ] Reject invalid blocks
- [ ] Request missing parents

**Minimal P2P sanity:**
```
Peer sends inv(block_hash)
  → We request getdata(block_hash)
  → Peer sends block
  → We validate
  → We connect if valid
```

**Files to check:**
- P2P message handlers
- Block download orchestration

---

### 6️⃣ **Mining Path End-to-End**
**Phase:** Mining verification
**Priority:** 🟡 **IMPORTANT**

**You must verify:**
```
mempool → CreateNewBlock → mine → submit → validate → connect
```

**Checklist:**
- [ ] Coinbase maturity enforced (100 blocks)
- [ ] Block template uses current UTXO
- [ ] Found block goes through **same validation** as network blocks
- [ ] Miner cannot bypass rules

**Regtest test:**
```bash
# Generate to address
generatetoaddress 101 <addr>

# Try to spend coinbase from block 1 (should fail - immature)
# Try to spend coinbase from block 1 after block 101 (should succeed)
```

**Files to check:**
- `src/mining/block_assembler.cpp`
- `src/daemon/mining.cpp`

---

### 7️⃣ **Basic RPC Sanity**
**Phase:** RPC essentials
**Priority:** 🟢 **NICE TO HAVE**

**You only need ~10 RPCs:**

**Mandatory:**
- [ ] `getblockchaininfo` - Current state
- [ ] `getblock` - Fetch block by hash
- [ ] `getblockhash` - Get hash by height
- [ ] `getbestblockhash` - Current tip
- [ ] `getrawtransaction` - Fetch tx
- [ ] `sendrawtransaction` - Submit tx
- [ ] `getmempoolinfo` - Mempool state
- [ ] `getblocktemplate` - Mining template
- [ ] `submitblock` - Submit mined block
- [ ] `generate` / `generatetoaddress` - Regtest mining

**Everything else can wait.**

**Files to check:**
- `src/rpc/methods_*.cpp`

---

## 🟡 SAFE TO POSTPONE (Without Breaking Correctness)

**Explicitly ignore for now:**
- ⏸️ Lightning
- ⏸️ Taproot covenants
- ⏸️ Advanced policy (RBF, package relay)
- ⏸️ Fee estimation sophistication
- ⏸️ Watchtowers
- ⏸️ Asset layers
- ⏸️ Snapshots
- ⏸️ Pruning (unless needed)
- ⏸️ Compact blocks optimizations

---

## 🧠 RECOMMENDED ORDER (Low Risk)

### Phase 1: Core Orchestration
1. ✅ Finish header → block orchestration
2. ✅ Verify ActivateBestChain exists and works
3. ✅ Close mempool ↔ reorg loop
4. ✅ Restart/resume correctness

### Phase 2: Validation
5. ✅ Mine a block on regtest
6. ✅ Reorg that block
7. ✅ Verify UTXO rollback works

### Phase 3: Polish (Only After Core Works)
8. ⏸️ Touch networking polish

---

## 🏁 REALITY CHECK

**You are not far.**

Most remaining work is:
- 🔧 Control flow
- 🔧 Orchestration
- 🔧 Glue logic

**The hard cryptographic and storage problems are already solved.**

---

## 🔍 NEXT ACTIONS (Choose One)

### Option A: Audit ActivateBestChain
**Goal:** Determine if you already implicitly have it

**Action:**
```bash
grep -rn "ActivateBestChain\|ConnectTip\|DisconnectTip" \
  src/consensus/ src/daemon/
```

**If found:** Audit for correctness
**If missing:** Implement as Priority 1

---

### Option B: Bitcoin Core Parity Checklist
**Goal:** Produce a "L1 parity checklist"

**Scope:** Only Layer 1 (base Bitcoin protocol)
- Block validation rules
- UTXO semantics
- Consensus rules
- P2P message flow

**Output:** Checklist document showing gaps

---

### Option C: Regtest Torture Script
**Goal:** Design a test that validates correctness

**Test scenarios:**
1. Mine 101 blocks
2. Create transactions
3. Mine them into blocks
4. Trigger reorg (mine competing chain)
5. Verify UTXO rollback
6. Restart node mid-test
7. Verify persistence

**Output:** Automated test script

---

### Option D: Define Clean Phase M.1
**Goal:** Scope next phase after M.0

**Focus:**
- Mempool ↔ block loop closure
- Reorg safety
- Double-spend prevention

**Exclude:**
- Fee policies
- Advanced mempool features

**Output:** Phase M.1 specification

---

## 📊 CURRENT STATUS SUMMARY

```
✅ Phase M.0: COMPLETE & LOCKED FOREVER
🔴 Phase H: Needs completion (header orchestration)
🔴 Core: ActivateBestChain verification needed
🔴 M.1 Core: Mempool loop closure needed
🔴 Restart: Persistence verification needed
🟡 Mining: End-to-end verification needed
🟡 RPC: Basic 10 RPCs sanity check needed
```

---

## 🎯 RECOMMENDATION

**Start with Option A: Audit ActivateBestChain**

This is the single most critical function. If it's missing or broken, everything else is meaningless.

**Then move to:** Regtest torture script (Option C)

This will reveal all the gaps in one systematic test.

---

## 📝 NOTES

**Phase M.0 Achievement:**
- This was the foundation - identity correctness
- Everything else builds on this
- With M.0 locked, you can now focus on orchestration

**Why this order matters:**
- Header sync drives everything
- ActivateBestChain is the orchestrator
- Mempool loop enables mining
- Persistence enables production use
- Only then does P2P polish matter

**Key insight:**
> "The hard problems are solved. What remains is making them work together."

---

**Next Step:** Choose an option (A, B, C, or D) and I can help execute it.
