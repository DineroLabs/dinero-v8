# Bitcoin Mining Documentation - Canonical Reading List

**Purpose:** Authoritative sources for understanding Bitcoin mining and ASERT difficulty adjustment
**Date:** 2025-12-21
**Context:** DineroCoin ASERT implementation verification

---

## 🟩 Tier 0 — Primary Canonical Sources (Authoritative)

These are **the law**, not tutorials.

### 1️⃣ Bitcoin Core Source Code (Mining + Difficulty)

**Repository:** https://github.com/bitcoin/bitcoin

#### Difficulty / Consensus
- `src/pow.cpp` - Difficulty calculation logic
- `src/pow.h` - PoW interfaces

**Key Functions:**
- `GetNextWorkRequired()` - Difficulty adjustment
- `CheckProofOfWork()` - Block validation

**Why Read This:**
- Shows correct consensus wiring
- Demonstrates difficulty as pure consensus, not mining logic
- Validates separation of concerns

**DineroCoin Equivalent:**
```
Bitcoin Core              DineroCoin
─────────────            ──────────────
src/pow.cpp        →     consensus/pow.cpp
GetNextWorkRequired() →  GetNextASERTWorkRequired()
```

**✅ Validation:** DineroCoin correctly isolates difficulty in consensus layer

#### Mining Logic
- `src/mining.cpp` - Block template creation
- `src/node/miner.cpp` - Mining coordination

**Key Functions:**
- `BlockAssembler::CreateNewBlock()` - Block template generation
- Coinbase construction rules
- Mempool transaction selection

**DineroCoin Equivalent:**
```
Bitcoin Core              DineroCoin
─────────────            ──────────────
src/mining.cpp     →     mining/block_assembler.cpp
BlockAssembler     →     BlockAssembler
CreateNewBlock()   →     CreateBlock()
```

**✅ Validation:** DineroCoin implements equivalent mining logic

---

### 2️⃣ ASERT Difficulty Specification (Bitcoin Cash → eCash)

**⚠️ CRITICAL:** ASERT is **NOT a Bitcoin Core invention**. Bitcoin Core uses legacy difficulty retarget.

#### The Authoritative ASERT Spec

**Repository:** https://github.com/bitcoin-cash-node/bitcoin-cash-node

**📄 MUST READ:** `doc/asert.md`
**URL:** https://github.com/bitcoin-cash-node/bitcoin-cash-node/blob/master/doc/asert.md

**This Document Explains:**
1. Exact mathematical formula
2. Fixed-point arithmetic requirements
3. Anchor block concept
4. Drift resistance properties
5. Why ASERT is stable under hashrate oscillations

**📌 THIS IS THE SPEC YOU IMPLEMENT AGAINST** — not blog posts, not whitepapers.

#### Reference Implementation

**Files to Study:**
- `src/pow/aserti3-2d.cpp` - ASERT algorithm implementation
- `src/pow/aserti3-2d.h` - ASERT interface

**Key Functions:**
```cpp
// Bitcoin Cash Node reference:
uint32_t GetNextASERTWorkRequired(
    const CBlockIndex* pindexPrev,
    const CBlockHeader* pblock,
    const Consensus::Params& params
)
```

**DineroCoin Equivalent:**
```
Bitcoin Cash Node           DineroCoin
─────────────────          ──────────────
src/pow/aserti3-2d.cpp  →  consensus/pow.cpp (ASERT section)
GetNextASERTWorkRequired() → GetNextASERTWorkRequired()
```

**✅ Validation Required:** Compare DineroCoin's ASERT math against this reference implementation

---

### 3️⃣ ASERT Whitepaper (Conceptual Foundation)

**📄 "ASERT: Absolutely Scheduled Exponentially Rising Targets"**
**Author:** Mark Lundeberg

**Purpose:** Conceptual understanding, not coding reference

**Explains:**
- Why ASERT exists (timestamp attack resistance)
- Why it fixes Bitcoin Cash's oscillation problem
- Mathematical properties
- Stability guarantees

**Use Case:** Understanding tradeoffs, not implementation details

---

## 🟨 Tier 1 — Bitcoin Protocol Documentation (Mining Semantics)

These explain how mining works at the protocol level.

### 4️⃣ Bitcoin Developer Reference (RPC-level)

**📄 URL:** https://developer.bitcoin.org/

**Focus Sections:**
1. **Mining**
   - Block construction rules
   - Coinbase requirements
   - Difficulty encoding

2. **getblocktemplate**
   - RPC contract specification
   - Block template format
   - Mining pool protocol

**DineroCoin Coverage:**
```
✅ getblocktemplate RPC implemented
✅ Coinbase rules enforced
✅ Merkle root correctness verified
```

**Use Case:** Confirms your RPC contract is correct, doesn't teach internals

---

### 5️⃣ BIPs (Bitcoin Improvement Proposals)

**📄 URL:** https://github.com/bitcoin/bips

**Mining-Relevant BIPs:**

| BIP | Title | Relevance |
|-----|-------|-----------|
| [BIP 22](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki) | getblocktemplate | Mining pool protocol |
| [BIP 23](https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki) | Block proposal | Pool coordination |
| [BIP 34](https://github.com/bitcoin/bips/blob/master/bip-0034.mediawiki) | Height in coinbase | Consensus rule |
| [BIP 141](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki) | SegWit weight | Block limits |
| [BIP 143](https://github.com/bitcoin/bips/blob/master/bip-0143.mediawiki) | SegWit sighash | Transaction signing |
| [BIP 340](https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki) | Schnorr signatures | Taproot (DineroCoin uses) |
| [BIP 341](https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki) | Taproot | Script validation |

**Why Read These:**
- Explains why block template rules exist
- Shows consensus requirements
- Documents protocol evolution

**DineroCoin Status:**
```
✅ BIP 34: Block height in coinbase implemented
✅ BIP 340/341: Taproot support implemented
🔄 BIP 141/143: SegWit (if applicable)
```

---

## 🟦 Tier 2 — Deep Explanations (Best Technical Writing)

Not "official", but industry gold standard.

### 6️⃣ Bitcoin Optech (Highly Recommended)

**📄 URL:** https://bitcoinops.org/

**Search Topics:**
- "mining"
- "difficulty adjustment"
- "ASERT"
- "block template"

**Why Read:**
- Explains edge cases
- Documents tradeoffs
- Industry best practices

**Use Case:** Understanding nuances after reading specs

---

### 7️⃣ Mastering Bitcoin (Andreas Antonopoulos)

**📄 URL:** https://github.com/bitcoinbook/bitcoinbook

**Chapters to Read:**
- **Chapter 10:** Mining and Consensus
- **Chapter 11:** Bitcoin Security

**Why Read:**
- Mental model alignment
- Big-picture understanding
- Historical context

**Use Case:** Conceptual foundation, not implementation guide

---

## 🧠 DineroCoin Implementation Mapping

### Current Architecture (Validated as Correct)

| Concept | DineroCoin Location | Status |
|---------|---------------------|--------|
| **Difficulty as Consensus** | `consensus/pow.*` | ✅ Correctly isolated |
| **ASERT Anchor** | Chain params | ✅ Implemented |
| **Block Template** | `mining/block_assembler.cpp` | ✅ Implemented |
| **Coinbase Rules** | Consensus layer | ✅ Enforced |
| **Wallet-Mining Boundary** | Separated | ✅ Clean separation |
| **GetNextWorkRequired()** | `GetNextASERTWorkRequired()` | 🔄 Needs verification against spec |

**Architecture Grade:** ✅ **Ahead of many altcoins**

---

## 📋 Verification Checklist

Use this to audit DineroCoin's ASERT implementation:

### Phase 1: Spec Compliance
- [ ] Read Bitcoin Cash Node `doc/asert.md`
- [ ] Compare math formula line-by-line
- [ ] Verify fixed-point arithmetic matches
- [ ] Check anchor block handling
- [ ] Validate edge cases (genesis, reorg, etc.)

### Phase 2: Reference Code Comparison
- [ ] Read `src/pow/aserti3-2d.cpp` from Bitcoin Cash Node
- [ ] Compare `GetNextASERTWorkRequired()` implementations
- [ ] Verify integer overflow handling
- [ ] Check rounding behavior
- [ ] Validate difficulty encoding/decoding

### Phase 3: Consensus Rules
- [ ] Verify difficulty is consensus-critical
- [ ] Check mining logic doesn't affect consensus
- [ ] Validate block acceptance rules
- [ ] Test reorg scenarios

### Phase 4: Edge Cases
- [ ] Test with extreme hashrate changes
- [ ] Test timestamp edge cases
- [ ] Test anchor block transitions
- [ ] Verify no timestamp manipulation vectors

---

## 🚨 Critical Warnings

### 1. No Single Source of Truth
> **Reality:** Bitcoin documentation is distributed across:
> - Source code (authoritative)
> - BIPs (protocol specs)
> - Developer docs (reference)
> - Technical writing (explanations)

**Anyone claiming "one PDF" is selling a tutorial, not providing specs.**

### 2. ASERT ≠ Bitcoin Core
> **Bitcoin Core uses:** Legacy difficulty retarget
> **ASERT comes from:** Bitcoin Cash / eCash

**Implication:** Read Bitcoin Cash Node docs, not Bitcoin Core, for ASERT specifics.

### 3. Code is Law
> **In consensus systems:** Implementation IS the specification

**Implication:** When in doubt, read the reference implementation code.

---

## 📚 Reading Order (Recommended)

### For Understanding ASERT:
1. Bitcoin Cash Node `doc/asert.md` (spec)
2. Bitcoin Cash Node `src/pow/aserti3-2d.cpp` (reference code)
3. ASERT whitepaper by Mark Lundeberg (conceptual)
4. Compare against DineroCoin implementation

### For Understanding Bitcoin Mining:
1. Bitcoin Core `src/pow.cpp` (difficulty)
2. Bitcoin Core `src/mining.cpp` (block assembly)
3. BIP 22/23 (getblocktemplate)
4. BIP 34 (height in coinbase)
5. Developer.bitcoin.org (RPC reference)

### For Mental Models:
1. Mastering Bitcoin Chapters 10-11
2. Bitcoin Optech articles
3. Historical context from Bitcoin talk forums

---

## 🎯 Next Steps for DineroCoin

### Immediate Actions:
1. **Compare ASERT implementation** against Bitcoin Cash Node reference
2. **Verify math formula** matches `doc/asert.md` exactly
3. **Test edge cases** (genesis, extreme hashrate changes)
4. **Document deviations** (if any) from reference implementation

### Long-term:
1. **Maintain consistency** with Bitcoin Cash Node ASERT updates
2. **Document anchor block** choice and rationale
3. **Test against real-world** hashrate patterns
4. **Fuzz test** difficulty calculation

---

## 📖 Quick Reference Links

### Primary Sources
- Bitcoin Core: https://github.com/bitcoin/bitcoin
- Bitcoin Cash Node: https://github.com/bitcoin-cash-node/bitcoin-cash-node
- BIPs: https://github.com/bitcoin/bips

### Documentation
- Developer Reference: https://developer.bitcoin.org/
- Bitcoin Optech: https://bitcoinops.org/
- Mastering Bitcoin: https://github.com/bitcoinbook/bitcoinbook

### ASERT Specific
- ASERT Spec: https://github.com/bitcoin-cash-node/bitcoin-cash-node/blob/master/doc/asert.md
- Reference Code: `bitcoin-cash-node/src/pow/aserti3-2d.cpp`

---

## 🏆 Key Takeaway

> **DineroCoin's architecture is sound.**
> Difficulty is correctly isolated as consensus.
> Mining logic is properly separated.
> ASERT implementation needs verification against Bitcoin Cash Node reference.

**Confidence Level:** High architecture, pending ASERT math audit

---

**Document Status:** Reference guide for DineroCoin mining system verification
**Last Updated:** 2025-12-21
**Maintainer:** Development team
