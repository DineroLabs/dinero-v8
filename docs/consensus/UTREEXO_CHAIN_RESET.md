# Utreexo Phase 3: Intentional Chain Reset

## ⚠️ CONSENSUS-BREAKING HARD FORK

**Phase:** Utreexo Phase 3 - Stateless Validation
**Activation Date:** January 9, 2026 (Regtest/Devnet)
**Status:** **INTENTIONAL CHAIN RESET** - No Migration Path

---

## Executive Summary

Utreexo Phase 3 introduces **fundamental block format changes** that are incompatible with pre-Utreexo blocks:

1. **Block header extension:** 80 bytes → 112 bytes (+32-byte Utreexo commitment)
2. **PoW domain change:** Hash covers all 112 bytes (not just 80)
3. **New genesis block:** Regtest genesis hash updated to 112-byte format

**Migration strategy:** Clean chain reset (NO attempt to replay or convert pre-Utreexo blocks)

---

## Why No Migration Path?

### Technical Impossibility

**Problem:** Pre-Utreexo blocks do NOT contain Utreexo proofs.

Pre-Utreexo Block:
```
Header (80 bytes):
  version || prev_hash || merkle_root || time || bits || nonce

Body:
  transactions (no Utreexo data)
```

Post-Utreexo Block:
```
Header (112 bytes):
  version || prev_hash || merkle_root || time || bits || nonce || utreexo_commitment

Body:
  transactions
  utreexo_data:  <-- THIS DOESN'T EXIST IN OLD BLOCKS
    ├─ accumulator_root_before
    ├─ spend_proof (batched Merkle proof)
    └─ spent_outputs (stateless validation data)
```

**Cannot reconstruct:** Utreexo proofs require the UTXO set state at block application time. Once the chain has progressed, historical UTXO set states are lost.

**Fundamental incompatibility:** Even if we could reconstruct proofs, the PoW hash domain changed. Old blocks hash 80 bytes; new blocks hash 112 bytes. These are cryptographically incompatible.

---

## Design Decision: Clean Reset

### Intentional Choice

**Option A:** Complex migration with UTXO set snapshots + proof reconstruction
- Requires storing historical UTXO sets
- Expensive computation to generate proofs
- Fragile and error-prone
- Still can't fix PoW hash domain mismatch

**Option B (chosen):** Clean chain reset from genesis
- Simple and correct
- No hybrid logic (pure Utreexo from block 0)
- No technical debt
- Testable and auditable

**Verdict:** Option B chosen for simplicity and correctness.

---

## What This Means

### For Regtest/Devnet (Current Deployment)

✅ **Chain reset is acceptable:**
- No production value at stake
- Test environments expect resets
- Clean slate for Utreexo testing

### For Testnet (Future Deployment)

✅ **Chain reset is acceptable:**
- Testnet resets are common
- Allows testing full Utreexo activation

⚠️ **Coordination required:**
- Announce reset date in advance
- Provide genesis block parameters
- Update network seeds

### For Mainnet (Future Deployment - NOT YET)

❌ **Chain reset is NOT acceptable:**
- Would destroy all mainnet value
- Not compatible with live network

**Mainnet strategy (when ready):**
1. Deploy Utreexo as optional feature first
2. Run hybrid mode (Utreexo + full UTXO set)
3. Gradually migrate without chain reset
4. Require multi-year planning

**Current status:** Phase 3 is experimental and NOT mainnet-ready.

---

## Explicit Consensus Rules

### Rule 1: All Blocks MUST Use 112-Byte Headers

```cpp
// Consensus enforcement (include/consensus/header_consensus.h)
if (header_size != 112) {
    return error("invalid-header-size");
}
```

**Effect:** 80-byte headers are REJECTED at consensus layer.

### Rule 2: PoW MUST Cover Full 112 Bytes

```cpp
// Mining enforcement (tools/dinero_miner.cpp)
sha256d(header.data(), 112, hash);  // Hash all 112 bytes
```

**Effect:** Miners cannot manipulate Utreexo commitment without recomputing PoW.

### Rule 3: Genesis Block Uses 112-Byte Format

```cpp
// New regtest genesis hash
const string REGTEST_GENESIS = "ae5aaabe923a716ffb096f5acf2ba97daca8bbd909c0e62e8c2d65504697cdfd";
```

**Effect:** Chain starts with Utreexo from block 0.

---

## Activation Timeline

### Phase 3 (Current - January 2026)

**Networks affected:**
- ✅ Regtest (immediate activation)
- ✅ Devnet (immediate activation)
- ❌ Testnet (not deployed yet)
- ❌ Mainnet (not deployed yet)

**Breaking changes:**
- Genesis hash changed
- Header size changed
- PoW domain changed

**Coordination:**
- All regtest/devnet nodes must upgrade simultaneously
- Old nodes cannot sync (header validation fails)
- No backward compatibility

### Phase 4 (Future - TBD)

**Goal:** Optimize forest serialization (delta-based undo)

**Network impact:** None (optimization, not consensus change)

**Deployment:** Can roll out without chain reset

### Mainnet Deployment (Future - Requires Planning)

**NOT READY:** Phase 3 implementation too expensive for mainnet.

**Prerequisites:**
1. Phase 4 delta-based undo (performance)
2. Extended testnet validation (6+ months)
3. Community consensus on migration strategy
4. Coordination with exchanges/pools

**Migration strategy for mainnet (when ready):**
- Hybrid mode (Utreexo optional, full UTXO set required)
- Gradual adoption over years
- NO chain reset (preserve value)

---

## Developer Guidance

### Running Regtest with Utreexo

**Fresh start (required):**
```bash
# Clean old chain data
rm -rf ~/.dinero/regtest/

# Start daemon (will create new genesis)
dinerod -regtest

# Verify genesis
dinero-cli -regtest getbestblockhash
# Expected: ae5aaabe923a716ffb096f5acf2ba97daca8bbd909c0e62e8c2d65504697cdfd
```

### Mining on Utreexo Chain

**Miner must use 112-byte headers:**
```bash
# Old miners will FAIL (hash wrong number of bytes)
# Use updated dinero-miner:
dinero-miner --address <your-address>

# Miner will automatically use 112-byte format
```

### Testing Block Validation

**Verify consensus rules:**
```bash
# Test 1: 80-byte header should REJECT
./test_invalid_header_size

# Test 2: 112-byte header should ACCEPT
./test_valid_header_size

# Test 3: PoW must cover full header
./test_pow_coverage
```

---

## FAQ

### Q: Can I replay my old regtest chain?

**A:** No. The genesis hash changed. You must start fresh.

### Q: Will my old wallet work?

**A:** Yes. Wallet keys are independent of block format. You can:
1. Export private keys from old chain
2. Import into new chain
3. Re-mine coinbase outputs (testnet/regtest only)

### Q: When will mainnet support Utreexo?

**A:** Not yet. Phase 3 is experimental. Mainnet requires:
- Phase 4 optimization (delta-based undo)
- Extended testing period (6-12 months)
- Community approval
- No chain reset strategy

### Q: Is this a "rug pull" or intentional break?

**A:** Intentional and documented:
- Regtest/devnet are test networks (resets expected)
- No production value affected
- Disclosed before deployment
- Part of research phase

---

## Comparison to Other Projects

### Bitcoin Core

**Approach:** Conservative (no breaking changes)
**Utreexo status:** Not activated (research only)
**Migration:** Would require soft fork or very long hybrid period

### DineroCoin

**Approach:** Research-first (experimental testnet)
**Utreexo status:** Phase 3 active (regtest/devnet)
**Migration:** Clean reset for test networks

**Advantage:** Can iterate faster without production constraints
**Trade-off:** No mainnet deployment yet

---

## Conclusion

Utreexo Phase 3 activation **intentionally requires a clean chain reset** for fundamental technical reasons:

1. **Block format incompatibility** (80 vs 112 bytes)
2. **PoW domain change** (hash coverage)
3. **Missing Utreexo proofs** in old blocks (cannot reconstruct)

**This is acceptable for regtest/devnet** (test environments) but **NOT acceptable for mainnet** (production).

**Mainnet deployment requires a different strategy** (hybrid mode, no reset) when Phase 4 is ready.

---

**Document Version:** 1.0
**Last Updated:** January 9, 2026
**Status:** ACTIVE (Regtest/Devnet)
**Mainnet Status:** NOT READY
