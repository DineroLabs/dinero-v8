# 🔒 CONSENSUS LOCKED — DO NOT MODIFY

**Date:** January 29, 2026
**Protocol:** Dinero Post-Utreexo v2.0.0
**Genesis Hash:** `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74`

---

## 📜 CONSENSUS AUTHORITY

**Consensus Authority: Code + Genesis Hash + This Document**

⚠️ **NO external announcement, blog post, README, or documentation can override consensus unless all three components agree:**
1. The code (consensus-critical files)
2. The genesis hash (00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74)
3. This document (CONSENSUS_LOCKED.md)

This is how Bitcoin survived forks. This is how Dinero will survive.

---

## ⚠️ CRITICAL WARNING

The following code is **CONSENSUS-CRITICAL** and **IMMUTABLE**.

**Any modification to these systems requires a HARD FORK.**

---

## 🚫 FORBIDDEN MODIFICATIONS

### DO NOT TOUCH:

1. **Genesis Block Parameters**
   - File: `src/consensus/chainparams_impl.cpp`
   - Lines: 33-80 (mainnet genesis block)
   - Any change = different genesis hash = incompatible chain

2. **Compact Target Conversion**
   - Files:
     - `include/consensus/pow_compact.h`
     - `include/consensus/pow_difficulty_helpers.h`
     - `include/dinero/core/consensus/pow_compact.h`
     - `include/dinero/core/consensus/pow_difficulty_helpers.h`
   - Functions: `TargetFromBitsBE()`, `compact_to_target()`
   - **CRITICAL:** Mantissa mask MUST be `0x00ffffff` (24 bits)
   - **CRITICAL:** Index calculation MUST be `idx = 32 - exp`

3. **Block Header Serialization**
   - File: `src/primitives/block.cpp`
   - Function: `BlockHeader::Serialize()`
   - **CRITICAL:** Header size MUST be 128 bytes (Post-Utreexo)
   - Format: version(4) + prevHash(32) + merkleRoot(32) + time(8) + bits(4) + nonce(4) + utreexo(32) + reserved(12)

4. **Genesis Hash References**
   - Must ALWAYS be: `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74`
   - Found in: chainparams, genesis_init, blockchain, mining, subsidy, ASERT

---

## ✅ WHAT YOU CAN MODIFY

These systems are **NON-CONSENSUS** and safe to change:

- RPC interface
- GUI
- P2P message handling (as long as block/tx format unchanged)
- Wallet logic
- Mining optimization (as long as block validation unchanged)
- Logging and debugging
- Performance improvements (as long as results identical)

---

## 🔐 VERIFICATION INVARIANT

**This MUST always be true:**

```bash
./dinero-cli getblockhash 0
# Returns: "00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74"
```

**If this EVER changes → the network is BROKEN.**

### Startup Invariant Check (ENFORCED)

**Added:** December 12, 2024

The node now performs a **mandatory startup check** that verifies:
```cpp
if (db_genesis_hash != code_genesis_hash) {
    std::abort();  // FATAL: Refuse to start
}
```

**This prevents:**
- Accidental mixed binaries (dev vs. production)
- Database from wrong network (mainnet vs. testnet)
- Silent consensus drift (code changed, DB didn't)
- "Boots but behaves weirdly" states

**Location:** `src/daemon/services/chainstate_service.cpp:151-194`

**Result:** Node will **immediately abort** if database genesis doesn't match code genesis.

---

## 📋 PRE-MODIFICATION CHECKLIST

Before modifying ANY code, ask yourself:

1. **Does this affect block hashing?** → FORBIDDEN
2. **Does this affect transaction serialization?** → FORBIDDEN
3. **Does this affect difficulty calculation?** → FORBIDDEN
4. **Does this affect genesis block?** → FORBIDDEN
5. **Does this affect header format?** → FORBIDDEN

If the answer to ANY question is YES → **DO NOT PROCEED** without:
- Full network upgrade coordination
- Hard fork announcement
- Community consensus
- At least 6 months notice

---

## 🧾 AUDIT TRAIL

### Consensus Bugs Fixed (Dec 12, 2024):

1. **Mantissa Mask Bug**
   - Wrong: `bits & 0x007fffff`
   - Right: `bits & 0x00ffffff`
   - Impact: Incorrect difficulty targets

2. **Index Calculation Bug**
   - Wrong: `idx = 32 - (exp - 3)`
   - Right: `idx = 32 - exp`
   - Impact: Wrong byte placement in target

3. **Header Size Bug**
   - Wrong: 80 bytes (legacy Bitcoin)
   - Right: 128 bytes (Post-Utreexo with 64-bit timestamp)
   - Impact: Wrong genesis hash computation

All bugs fixed. Genesis validated. Protocol locked.

---

## 📜 CANONICAL FILES (NEVER DELETE)

These files are **PERMANENT PROTOCOL ARTIFACTS**:

- `genesis_post_utreexo.json`
- `GENESIS_V2_FINAL.json`
- `docs/launch/GENESIS_V2_FINAL.json`
- `docs/launch/GENESIS_V2_FINAL.txt`
- `docs/launch/CONSENSUS_FIXES_SUMMARY.md`
- `tools/verify_genesis.sh`
- **THIS FILE** (`CONSENSUS_LOCKED.md`)

**SHA256 of genesis JSON:** `49976d087887b9e04b787368baa80be6085b7b31cb2780aa6dd492f6a2e160b5`

---

## 🎯 LAUNCH DATE

**November 25, 2025 00:00:00 UTC**

- Timestamp: `1772496000`
- nBits: `0x1d31ffce` (unified difficulty, 50x easier than Bitcoin)
- Nonce: `2954325912`

**After this timestamp, NO changes to consensus code are permitted without a hard fork.**

---

## 🚨 EMERGENCY CONTACTS

If you discover a consensus bug AFTER launch:

1. **DO NOT push a "fix" to production**
2. **DO NOT announce the bug publicly** (security risk)
3. **Contact core team immediately**
4. **Coordinate hard fork if necessary**

A consensus bug in production requires:
- Immediate coordination
- Network-wide upgrade
- Possible rollback planning

**Better to be slow than to cause a chain split.**

---

## 📖 RECOMMENDED READING

For anyone modifying this codebase:

1. Bitcoin Compact Target Format (BIP)
2. Utreexo Accumulator Specification
3. ASERT Difficulty Adjustment (Bitcoin Cash spec)
4. Consensus-critical code guidelines

**If you don't understand consensus, DO NOT MODIFY CONSENSUS CODE.**

---

**Sealed:** January 29, 2026
**Protocol:** Dinero Post-Utreexo v2.0.0
**Status:** LOCKED

**⚠️ THIS DOCUMENT IS BINDING ⚠️**
