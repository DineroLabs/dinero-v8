# DineroCoin Governance Established

**Date:** 2025-12-13
**Status:** MILESTONE - Transition from Development to Protocol Engineering
**Authority:** Protocol Governance Framework

---

## Executive Summary

Today marks the transition from **development chaos** to **protocol-level governance** for DineroCoin. We've established the canonical frameworks that govern how this blockchain evolves, releases software, and coordinates network upgrades.

**Three documents now form the governance spine:**

1. **Consensus_Versioning_Policy.md** - What triggers version changes
2. **RELEASE_PROCESS.md** - How releases are created and distributed
3. **PHASE_3_4A_COMPLETE.md** - Technical achievement baseline

---

## What Changed Today

### Before (Development Chaos)

```
❌ Ad-hoc milestone tags (v0.9.0-zombie-eliminated)
❌ No clear definition of what triggers v1.0 vs v2.0
❌ No formal release process
❌ No consensus change coordination plan
❌ Version bumps = "when it feels right"
```

### After (Protocol Governance)

```
✅ Canonical versioning policy (blockchain-specific semantics)
✅ Clear boundaries: v0.x (dev), v1.x (mainnet), v2.x (hard forks)
✅ Formal release process (dev, RC, mainnet)
✅ Consensus change process (soft fork, hard fork activation)
✅ Version bumps = "when policy criteria met"
```

---

## Governance Documents

### 1. Consensus Versioning Policy

**Location:** `Consensus_Versioning_Policy.md`
**Authority:** Protocol Specification
**Status:** CANONICAL (v1.0)

**Defines:**
- What constitutes MAJOR (hard fork), MINOR (soft fork/features), PATCH (bug fixes)
- Version ranges: v0.x (pre-production), v1.x (mainnet), v2.x (hard forks)
- Frozen parameters at v1.0.0 (header size, MAX_MONEY, halving schedule)
- Feature roadmap: v0.10.0 → v1.0.0 → v1.5.0+
- Consensus change process (BIP9-style activation)

**Key Principle:**
> "Consensus versions only move forward. Tags are never rewritten.
>  This policy explains the guard rails, it does not rewrite history."

**Historical Note:**
- Tags before 2025-12-13 may not follow policy (preserved as-is)
- `v0.9.0-zombie-eliminated` = pre-policy architectural milestone
- **v0.10.0** = First release fully governed by this policy

---

### 2. Release Process

**Location:** `RELEASE_PROCESS.md`
**Authority:** Operational Specification
**Status:** CANONICAL (v1.0)

**Defines:**
- How to create development releases (v0.x.x)
- How to create release candidates (v0.x.x-rc1)
- How to create mainnet releases (v1.x.x)
- Testing requirements (unit, integration, stress, security)
- Build process (source, binaries, signatures)
- Emergency release process (critical fixes)
- Post-release checklist

**Testing Thresholds:**
- Development: `make test` passes, basic functional
- RC: Extended suite, 7-day testnet uptime, multi-node
- Mainnet: RC + 30 days + security audit + benchmarks

**Binary Distribution:**
- Source tarball (always)
- Linux/macOS/Windows binaries (RC and mainnet only)
- SHA256 checksums + GPG signatures
- Gitian builds (v1.0.0+ for reproducibility)

---

### 3. Phase 3 & 4A Completion

**Location:** `PHASE_3_4A_COMPLETE.md`
**Authority:** Technical Achievement Baseline
**Status:** Historical Record

**Documents:**
- Block indexing foundation (Height/Hash/TXID)
- Reorg safety implementation (UTXO + TX index rollback)
- P2P sync handlers (getheaders/getblocks)
- Critical bug fix (TX index rollback in DisconnectBlock)
- Utreexo integration status (core protocol, not extension)
- Architecture validation (single-chain authority model)

**This is the technical baseline for v0.10.0+**

---

## Version Semantics (Blockchain Context)

### v0.x.x - Pre-Production Development

**Current Phase:** v0.9.0 → v0.15.0
**Network:** Testnet/Regtest only
**Guarantees:** NONE - Breaking changes allowed

**Roadmap:**
- v0.10.0 - RPC Layer Complete (Phase 4B)
- v0.11.0 - Mempool Hardening
- v0.12.0 - Wallet Feature Complete
- v0.13.0 - P2P Network Hardening
- v0.14.0 - Mining & Block Assembly
- v0.15.0 - Security Audit & Testnet Launch

---

### v1.x.x - Production Mainnet (Consensus FROZEN)

**First Release:** v1.0.0 (Mainnet Launch)
**Network:** Production mainnet
**Guarantees:** Full backward compatibility within v1.x

**Frozen Parameters (Cannot Change Without Hard Fork):**
- Block header: 128 bytes (80 Bitcoin + 32 Utreexo + 4 timestamp expansion + 12 reserved)
- PoW algorithm: Argon2id (parameters locked)
- MAX_MONEY: 97,850,000 DIN
- Halving interval: 420,000 blocks (~4 years)
- Block time target: 150 seconds
- Genesis block hash: `0x1ad3...` (already locked)
- Genesis timestamp: 1727817600 (already locked)

**v1.x.x Progression (Post-Mainnet):**
- v1.0.0 - Mainnet launch
- v1.1.0 - Lightning Network (soft fork)
- v1.2.0 - Advanced script features (Taproot-style)
- v1.3.0 - Privacy enhancements
- v1.4.0 - Utreexo proof optimizations
- v1.5.0+ - Future features (cross-chain, sidechains)

---

### v2.x.x - Hard Forks (Consensus-Breaking Changes)

**Trigger:** Only for critical protocol changes
**Coordination:** 180-day notice minimum, 95% miner signaling
**Examples:** Post-quantum upgrade, PoW change, header format change

**Not Planned:** Supply cap change, consensus algorithm change

---

## Tagging Strategy: Option 3 (Conservative)

**Decision:** Keep existing pre-policy tags as-is. Do not rewrite history.

### Current State (Pre-Policy Era)

| Commit | Tag | Status |
|--------|-----|--------|
| a32e14b6 | `v0.9.0-zombie-eliminated` | ✅ Preserved (pre-policy milestone) |
| 7068f0b9 → 4470cb80 | (none) | Untagged (Phase 3 & 4A work) |
| 7e90cb89 → dd6b025f | (none) | Untagged (governance establishment) |

### Policy Era Begins

**Next Release:** v0.10.0 (Phase 4B - RPC Layer Complete)
- First release fully governed by Consensus Versioning Policy
- Clean boundary between development and policy-governed releases
- All subsequent releases follow MAJOR.MINOR.PATCH semantics

---

## Consensus Change Process

### Soft Fork (v1.x.0)

**Definition:** Restrictive change (old nodes still validate new blocks)
**Examples:** New script opcodes, witness commitments

**Activation Process:**
1. Proposal (BIP-style improvement proposal)
2. Implementation (feature flag on testnet)
3. Miner signaling (BIP9-style, 75% threshold)
4. Lock-in (2016-block window)
5. Activation (1-month grace period)

**Timeline:** 90 days notice minimum

---

### Hard Fork (v2.0.0)

**Definition:** Consensus-breaking change (incompatible with old nodes)
**Examples:** Block format change, PoW change, header size change

**Activation Process:**
1. Proposal phase (T-180 days, BIP-style)
2. Review phase (T-120 days, security audit, community review)
3. Activation phase (T-30 days, 95% miner signaling)
4. Activation height (T-0, flag-day activation)

**Coordination:**
- Exchange/wallet partnerships
- User communication
- Network monitoring
- Nodes not upgraded: forked off network

**Timeline:** 180 days notice minimum

---

## What This Means for Development

### From v0.10.0 Forward

**When Making Changes, Ask:**

1. **Does it change consensus validation rules?**
   - YES → MAJOR version (v2.0.0, requires hard fork)
   - NO → Continue to question 2

2. **Does it add features/APIs without breaking old nodes?**
   - YES → MINOR version (v1.x.0 or v0.x.0 in dev)
   - NO → Continue to question 3

3. **Does it fix a bug without changing behavior?**
   - YES → PATCH version (v1.x.x or v0.x.x in dev)
   - NO → Re-evaluate scope

**Critical Rule:**
> If a bug fix changes block/transaction validation logic, it's a MAJOR version (hard fork), not a patch.

---

## Release Checklist (Quick Reference)

### Development Release (v0.x.x)

1. [ ] Feature complete
2. [ ] Tests pass (`make test`)
3. [ ] Update `src/version.h`
4. [ ] Update `CHANGELOG.md`
5. [ ] Create annotated tag: `git tag -a v0.x.x -m "..."`
6. [ ] Push tag: `git push origin v0.x.x`

**Distribution:** Source code only (no binaries)

---

### Release Candidate (v0.x.x-rc1)

1. [ ] All development requirements
2. [ ] Build binaries (Linux/macOS/Windows)
3. [ ] Deploy to testnet (7+ days uptime)
4. [ ] Multi-node testing (3+ independent nodes)
5. [ ] Create GitHub release with binaries
6. [ ] Monitor for issues

**Distribution:** Source + binaries + SHA256 checksums

---

### Mainnet Release (v1.x.x)

1. [ ] All RC requirements
2. [ ] 30+ days testnet deployment
3. [ ] Security audit passed
4. [ ] Build and sign binaries (Gitian builds)
5. [ ] Update documentation (RPC docs, user guides)
6. [ ] Announce to community
7. [ ] Monitor network for 48 hours post-release

**Distribution:** Full package (binaries + docs + signatures + upgrade guide)

---

## Frozen Parameters at v1.0.0

These parameters **cannot change** without a hard fork (v2.0.0):

```cpp
// Block header format
constexpr size_t BLOCK_HEADER_SIZE = 128;  // 80 Bitcoin + 32 Utreexo + 4 timestamp expansion + 12 reserved

// Economics
constexpr uint64_t MAX_MONEY = 97'850'000 * COIN;  // 97.85M DIN
constexpr int HALVING_INTERVAL = 420'000;          // ~4 years

// Timing
constexpr int64_t TARGET_SPACING = 150;            // 2.5 minutes
constexpr int64_t TARGET_TIMESPAN = 1209600;       // 2 weeks

// Genesis (already locked)
const std::string GENESIS_HASH = "0x1ad3...";
const int64_t GENESIS_TIMESTAMP = 1727817600;

// PoW (locked at v1.0.0)
const std::string POW_ALGORITHM = "Argon2id";
const uint32_t ARGON2_TIME_COST = 2;
const uint32_t ARGON2_MEMORY_COST = 65536;
const uint32_t ARGON2_PARALLELISM = 1;
```

**Exit Criteria for v1.0.0 (Mainnet Launch):**
- [ ] All v0.15.0 features complete
- [ ] Security audit passed (external review)
- [ ] Testnet: 30+ days, 3+ independent nodes, 0 consensus bugs
- [ ] Economic consensus: Community approval
- [ ] Exchange partnerships confirmed
- [ ] Documentation complete (RPC docs, user guides)

---

## Mental Model: Curating History

### The Old Way (Developer Thinking)

```
"Let's tag this v1.0 because it feels complete."
"We can always change the version later."
"Tags are just pointers, we can rewrite them."
```

### The New Way (Protocol Engineering)

```
"v1.0.0 means consensus is FROZEN - are we ready for that commitment?"
"Once tagged, it's permanent. What does this version promise?"
"Tags are history. We curate, we don't rewrite."
```

**Key Insight:**
> You're no longer just building software — you're curating history.
> From here on, consistency beats perfection.

---

## What's Next

### Immediate (v0.10.0)

**Phase 4B: RPC Layer Complete**
- Wire getblock, getrawtransaction, getblockheader
- Wire getbestblockhash, getblockcount, getblockhash
- Test with dinero-cli
- Document RPC methods
- Create v0.10.0 release (first policy-governed release)

**Status:** First release under new governance framework

---

### Near-Term (v0.11.0 → v0.15.0)

**v0.11.0 - Mempool Hardening**
- Transaction relay (inv/getdata)
- Replace-by-fee (RBF)
- Mempool persistence
- Fee estimation

**v0.12.0 - Wallet Feature Complete**
- HD wallet (BIP32/BIP44)
- Coin selection
- Transaction batching

**v0.13.0 - P2P Network Hardening**
- DNS seeds
- Eclipse attack resistance
- Compact block relay
- Utreexo proof relay optimization

**v0.14.0 - Mining & Block Assembly**
- GetBlockTemplate enhancements
- Stratum v2 support
- Mining pool readiness

**v0.15.0 - Security Audit & Testnet**
- External security review
- Testnet launch (30-day minimum)
- Stress testing (10k+ tx/block)
- Economic parameter finalization

---

### Long-Term (v1.0.0+)

**v1.0.0 - Mainnet Launch**
- Consensus rules FROZEN
- First 100 blocks: Foundation premine
- Block 101+: Public mining begins

**v1.1.0+ - Post-Mainnet Evolution**
- Lightning Network (soft fork)
- Advanced script features
- Privacy enhancements
- Cross-chain capabilities

**v2.0.0+ - Future Hard Forks**
- Reserved for critical protocol changes only
- Post-quantum signature scheme (when needed)
- Not planned: Supply cap change, PoW change (unless critical)

---

## Document Authority

**These three documents form the canonical governance framework:**

1. **Consensus_Versioning_Policy.md** - CANONICAL
   - Changes require: GitHub issue, PR, community discussion, maintainer approval

2. **RELEASE_PROCESS.md** - CANONICAL
   - Changes require: GitHub issue, PR, release manager approval

3. **PHASE_3_4A_COMPLETE.md** - Historical Record
   - No changes (frozen achievement baseline)

**This document (GOVERNANCE_ESTABLISHED.md):**
- Status: Historical milestone marker
- Purpose: Explain the transition and governance framework
- Not subject to amendment (snapshot in time)

---

## Commits Establishing Governance

| Commit | Description |
|--------|-------------|
| 7e90cb89 | Establish canonical consensus versioning policy |
| e6ce28d8 | Add historical note and formalize v0.10.0 as Policy Era boundary |
| dd6b025f | Establish canonical release process for v0.10.0+ |

**Total:** 3 commits, 1,600+ lines of governance documentation

---

## Conclusion

**DineroCoin has crossed the line from development to protocol engineering.**

We're no longer just writing code - we're:
- Curating blockchain history
- Coordinating network upgrades
- Freezing consensus rules at v1.0.0
- Planning hard forks with 180-day notice
- Thinking in protocol versions, not just software versions

**The guard rails are in place. The roadmap is clear. The governance is established.**

**From v0.10.0 forward, we operate under canonical policy.**

---

**Principle:**
> Consensus versions only move forward. Tags are never rewritten.
> Consistency beats perfection. We curate history, we don't rewrite it.

**Status:** GOVERNANCE ESTABLISHED
**Date:** 2025-12-13
**Next Milestone:** v0.10.0 (Policy Era Begins)

---

**Generated:** 2025-12-13
**Contributors:** Claude Sonnet 4.5 + Human
**Status:** Historical Milestone
