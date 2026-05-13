# DineroCoin Consensus Versioning Policy

**Document Version:** 1.0
**Status:** CANONICAL
**Date:** 2025-12-13
**Authority:** Protocol Specification

---

## Executive Summary

This document defines the **canonical versioning policy** for DineroCoin, establishing clear rules for what constitutes a consensus-breaking change, network upgrade, or backward-compatible improvement. This is not a developer's guide - this is a **protocol-level specification** that governs network coordination, hard forks, and soft forks.

---

## Historical Note

**Tags created prior to the adoption of this policy (before 2025-12-13) may not strictly follow the semantic versioning rules defined herein.**

This policy applies strictly to all versions released **after its adoption**.

**Pre-Policy Tags (Development Experiments):**

The following tags exist on GitHub from pre-policy development and do NOT follow the semantics defined in this document. They are preserved as historical markers but should not be interpreted according to this policy:

- `v1.0.0`, `v1.0.1`, `v1.0.2`, `v1.0.3` - Early development releases (NOT mainnet, NOT consensus-frozen)
- `v1.1.0-rc1`, `v1.1.0-lightning-security`, `v1.1.1`, `v1.1.2` - Lightning integration experiments
- `v1.2.0` - Development milestone
- `v1.3.0`, `v1.3.1-mainnet-blockers` - Development milestones
- `v0.x.x` series - Various development tags

**Local-Only Tags (Deleted):**
- `v2.0.0-block1`, `v2.0.0-genesis` - Were local only, deleted before policy adoption to prevent confusion

**Important Clarification:**
- Pre-policy v1.x tags do NOT mean "mainnet with frozen consensus"
- They were development experiments before governance was established
- Under this policy, v1.0.0 will be the **actual mainnet launch** with frozen consensus
- Pre-policy tags remain on GitHub as historical artifacts but are superseded by this policy

**Policy Era Begins:**
- **v0.10.0** (2025-12-13) - First release fully governed by this Consensus Versioning Policy
- All subsequent releases must adhere to MAJOR.MINOR.PATCH semantics as defined below

**Principle:** Consensus versions only move forward. Tags are never rewritten. This policy explains the guard rails, it does not rewrite history.

---

## Semantic Versioning: Blockchain Context

DineroCoin follows **Semantic Versioning 2.0.0** with blockchain-specific interpretations:

```
MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

### Version Component Definitions

| Component | Triggers | Examples | Consensus Impact |
|-----------|----------|----------|------------------|
| **MAJOR** | Hard fork (consensus-breaking change) | Block format change, PoW algorithm change | ⚠️ BREAKING - Requires network-wide upgrade |
| **MINOR** | Soft fork or new features (backward-compatible) | New RPC methods, wallet features, performance improvements | ✅ COMPATIBLE - Old nodes can validate new blocks |
| **PATCH** | Bug fixes, security patches (no protocol change) | Fix reorg bug, fix mempool leak | ✅ COMPATIBLE - No consensus logic changes |
| **PRERELEASE** | Development builds (`-alpha`, `-beta`, `-rc1`) | `v1.0.0-rc1` | ⚠️ NOT PRODUCTION |
| **BUILD** | Build metadata (`+build123`, `+commit.abc123`) | `v1.0.0+build.20251213` | 📦 Informational only |

---

## Version Ranges: Lifecycle Phases

### Phase 1: Pre-Production Development (`v0.x.x`)

**Status:** Breaking changes allowed without coordination
**Network:** Testnet/Regtest only
**Guarantees:** NONE - Consensus rules can change freely

**Characteristics:**
- Foundation implementation phase
- Architecture experimentation permitted
- Database schema changes acceptable
- Genesis block may be regenerated
- No backward compatibility guarantees

**Current State (as of 2025-12-13):** `v0.9.x`

**Exit Criteria for v1.0.0:**
- ✅ Block acceptance and validation complete
- ✅ UTXO set management complete
- ✅ Reorg safety proven (all indexes roll back correctly)
- ✅ P2P sync (headers-first) functional
- ✅ RPC layer complete (getblock, getrawtransaction, etc.)
- ✅ Wallet: Send/receive transactions
- ✅ Mempool: TX relay and replacement
- ✅ Mining: Template generation and submission
- ⚠️ Security audit: External review of consensus code
- ⚠️ Testnet: 30+ days uptime with >3 independent nodes
- ⚠️ Economic finalization: Supply cap locked, halving schedule frozen

---

### Phase 2: Production Mainnet (`v1.x.x`)

**Status:** Consensus rules FROZEN unless hard fork
**Network:** Mainnet
**Guarantees:** Full backward compatibility within v1.x

**MAJOR (v1.x.0) - Hard Fork Required:**

Changes that **break consensus** and require ALL nodes to upgrade:

1. **Block Header Format Change**
   - Current: 128 bytes (80 Bitcoin + 32 Utreexo + 4-byte timestamp expansion + 12-byte reserved)
   - Example trigger: Adding new commitment field
   - Coordination: 90-day notice, activation height

2. **Transaction Format Change**
   - Adding new script opcodes that old nodes can't validate
   - Changing signature algorithm
   - Modifying UTXO commitment structure

3. **Consensus Rule Change**
   - PoW algorithm change (e.g., Argon2id parameter adjustment)
   - Block size/weight limit change
   - Difficulty adjustment algorithm change
   - Supply schedule change (MAX_MONEY, halving interval)

4. **Utreexo Commitment Change**
   - Changing accumulator structure
   - Adding new Utreexo proof requirements

**MINOR (v1.x.0) - Soft Fork or Feature:**

Changes that are **backward-compatible**:

1. **Soft Forks** (Restrictive Changes)
   - New block validation rules that make blocks MORE restrictive
   - Example: SegWit-style witness commitment (old nodes see valid blocks)
   - Requires miner majority (75%+ hashrate)

2. **RPC/API Additions**
   - New RPC methods (`getutxosetinfo`, `getblockstats`)
   - New websocket events
   - New wallet commands

3. **Performance Improvements**
   - Database optimization
   - Utreexo proof caching
   - Mempool indexing improvements
   - P2P bandwidth optimizations

4. **Non-Consensus Features**
   - Lightning Network integration
   - Wallet encryption improvements
   - Block explorer enhancements

**PATCH (v1.x.x) - Bug Fixes:**

Changes that **fix bugs without altering consensus**:

1. **Reorg Safety Fixes**
   - Example: TX index rollback bug (if found in production)
   - Requirement: Must not change validation outcome

2. **Memory Leaks / Performance Bugs**
   - Mempool memory management
   - RocksDB compaction tuning

3. **Security Fixes** (Non-Consensus)
   - RPC authentication bypass
   - P2P DoS vulnerabilities
   - Wallet key management issues

**Critical Rule:**
If a bug fix changes block/transaction validation logic, it's a **MAJOR** version (hard fork), not a patch.

---

### Phase 3: Protocol Evolution (`v2.0.0+`)

**Status:** Major architectural changes
**Trigger:** Fundamental protocol redesign
**Coordination:** 180-day notice minimum, testnet validation

**Examples of v2.0.0 Triggers:**

1. **Consensus Algorithm Change**
   - PoW → PoS transition
   - Hybrid consensus model
   - New signature scheme (e.g., post-quantum)

2. **Block Structure Overhaul**
   - Complete header format redesign
   - New merkle tree structure (e.g., Bitcoin-NG blocks)

3. **UTXO Model Change**
   - Account-based model (like Ethereum)
   - Utreexo → different accumulator

4. **Economic Model Change**
   - Supply cap change (requires extraordinary consensus)
   - Inflation schedule redesign

**Hard Fork Activation Process:**

1. **Proposal Phase** (T-180 days)
   - BIP-style improvement proposal
   - Reference implementation on testnet
   - Economic impact analysis

2. **Review Phase** (T-120 days)
   - Security audit
   - Community review
   - Miner signaling begins

3. **Activation Phase** (T-30 days)
   - 95% miner signaling threshold
   - Exchange/wallet coordination
   - User communication

4. **Activation Height** (T-0)
   - Block height where new rules activate
   - Nodes not upgraded: forked off network

---

## Feature Roadmap: Version Mapping

### Current State: v0.9.0 (Development)

**Completed (Phase 3 & 4A):**
- ✅ Block indexing (Height/Hash/TXID)
- ✅ Reorg safety (UTXO + TX index rollback)
- ✅ P2P sync (getheaders/getblocks)
- ✅ Single-chain authority model
- ✅ Utreexo integration (128-byte headers, 64-bit timestamp)

### Upcoming: v0.10.0 → v0.15.0 (Pre-Production)

**v0.10.0 - RPC Layer Complete**
- Wire getblock, getrawtransaction, getblockheader
- Wire getbestblockhash, getblockcount, getblockhash
- Wallet RPC: listunspent, listtransactions

**v0.11.0 - Mempool Hardening**
- Transaction relay (inv/getdata)
- Replace-by-fee (RBF) support
- Mempool persistence across restarts
- Fee estimation

**v0.12.0 - Wallet Feature Complete**
- HD wallet (BIP32/BIP44)
- Coin selection algorithms
- Transaction batching
- Address book

**v0.13.0 - P2P Network Hardening**
- Peer discovery (DNS seeds)
- Eclipse attack resistance
- Compact block relay (BIP152 equivalent)
- Utreexo proof relay optimization

**v0.14.0 - Mining & Block Assembly**
- GetBlockTemplate enhancements
- Stratum v2 support
- Mining pool readiness
- Difficulty adjustment validation

**v0.15.0 - Security Audit & Testnet**
- External security review
- Testnet launch (30-day minimum)
- Stress testing (10k+ tx/block)
- Economic parameter finalization

---

### Milestone: v1.0.0 (Mainnet Launch)

**Definition:** First production-ready release with frozen consensus rules

**Frozen Parameters (Cannot Change in v1.x):**
- Block header: 128 bytes (80 Bitcoin + 32 Utreexo + 4 timestamp expansion + 12 reserved)
- PoW: Argon2id (parameters locked)
- Supply cap: MAX_SUPPLY = 262,800,000 DIN
- Initial block reward: 100 DIN
- Halving interval: 1,314,000 blocks (5.0 years)
- Block time target: 120 seconds (2 minutes)
- ASERT half-life: 12 hours (43,200 seconds)
- Premine: 2,627,900 DIN (1% of max supply minus genesis)
- Genesis unspendable: 100 DIN
- Network Magic: 0xd9b4bef9
- Genesis block hash: `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74`
- Genesis motto: "Dinero: Real Money For Free People"
- Genesis time: 1772496000 (2026-03-03 00:00:00 UTC)

**Exit Criteria:**
- All v0.15.0 features complete
- Security audit passed
- Testnet: 30+ days, 3+ independent nodes, 0 consensus bugs
- Economic consensus: Community approval
- Exchange partnerships confirmed
- Documentation complete

**Activation:**
- Block 0: Genesis block (100 DIN unspendable, symbolic OP_RETURN)
- Block 1: Premine block (2,627,900 DIN)
- Block 2+: Public PoW mining begins (100 DIN initial reward)

---

### Post-Mainnet: v1.1.0 → v1.9.0 (Soft Forks & Features)

**v1.1.0 - Lightning Network**
- LN protocol integration
- Channel management
- Invoice generation/payment

**v1.2.0 - Advanced Script Features**
- Taproot-style aggregation (soft fork)
- Covenant support (OP_CHECKTEMPLATEVERIFY equivalent)
- Time-locked contracts

**v1.3.0 - Privacy Enhancements**
- CoinJoin integration
- Stealth addresses (non-consensus)
- Taproot multisig

**v1.4.0 - Utreexo Proof Optimizations**
- Compact proofs (soft fork)
- Proof caching improvements
- Bridge node infrastructure

**v1.5.0+ - Future Features**
- Cross-chain atomic swaps
- Sidechain support
- Advanced wallet features

---

### Future: v2.0.0+ (Hard Forks Only)

Reserved for **consensus-breaking changes** only:

**Potential v2.0.0 Triggers:**
- Post-quantum signature scheme (when needed)
- UTXO commitment structure change
- PoW algorithm upgrade (security necessity)
- Block structure redesign (if critical flaw found)

**Not Planned:** Supply cap change, consensus algorithm change

---

## Tagging Convention

### Git Tags Format

```
v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]
```

**Examples:**
- `v0.10.0` - RPC layer complete (development)
- `v0.15.0-rc1` - Release candidate 1
- `v1.0.0` - Mainnet launch
- `v1.2.0` - Lightning Network soft fork
- `v2.0.0` - (Hypothetical) Post-quantum upgrade

### Special Tags (Allowed in v0.x Only)

For development milestones that don't map to features:

```
v0.9.0-<milestone-name>
```

**Examples:**
- `v0.9.0-zombie-eliminated` - Blockchain stub deleted
- `v0.9.1-reorg-safety` - TX index rollback fixed
- `v0.9.2-indexing-complete` - Block indexing done

**DEPRECATED after v1.0.0** - Use proper MINOR versions instead.

---

## Tagging Strategy: Option 3 (Conservative, Professional)

**Decision:** Keep existing pre-policy tags as-is. Do not rewrite history.

### Current State (Pre-Policy Era)

| Commit | Tag | Status | Notes |
|--------|-----|--------|-------|
| a32e14b6 | `v0.9.0-zombie-eliminated` | ✅ Preserved | Pre-policy architectural milestone |
| 7068f0b9 | (none) | Untagged | P2P network wiring |
| cf8e3f47 | (none) | Untagged | TX indexing implemented |
| 9c0ceb6d | (none) | Untagged | getheaders/getblocks complete |
| ddb318ad | (none) | Untagged | **CRITICAL** - Reorg safety fix |
| 3c61b657 | (none) | Untagged | TX index reorg test |
| 4470cb80 | (none) | Untagged | Phase 3 & 4A documentation |
| 7e90cb89 | (none) | Untagged | Consensus versioning policy established |

### Policy Era Begins (v0.10.0+)

**Next Release: DineroCoin Core v0.10.0**
- **Policy Status:** First release fully governed by Consensus Versioning Policy
- **Scope:** Phase 4B - RPC Layer Complete (getblock, getrawtransaction, etc.)
- **Significance:** Clean boundary between pre-policy development and policy-governed releases

**From v0.10.0 Forward:**
- ✅ All tags strictly follow MAJOR.MINOR.PATCH semantics
- ✅ Consensus versions only move forward
- ✅ Tags are never rewritten
- ✅ Policy explains guard rails, does not rewrite history

**Principle:** Consistency beats perfection. We're curating history, not just building software.

---

## Consensus Change Process

### Step 1: Proposal (BIP-Style)

**Required Documentation:**
- Problem statement
- Proposed solution
- Backward compatibility analysis
- Reference implementation
- Test vectors

### Step 2: Implementation (Testnet)

**Requirements:**
- Feature flag for activation
- Testnet deployment (minimum 30 days)
- Independent node testing

### Step 3: Activation (Mainnet)

**Soft Fork (v1.x.0):**
- Miner signaling (BIP9-style)
- 75% threshold over 2016-block window
- 1-month grace period after lock-in
- Activation at predetermined height

**Hard Fork (v2.0.0):**
- 180-day notice minimum
- 95% miner signaling
- Exchange/wallet coordination
- Flag-day activation (specific block height)

### Step 4: Post-Activation

**Monitoring:**
- Network split detection
- Orphan rate monitoring
- User adoption tracking

---

## Version Query (RPC)

Nodes must expose version information via RPC:

```json
{
  "version": 10000,        // 1.0.0 = 10000, 1.2.3 = 10203
  "protocolversion": 1,    // Consensus protocol version
  "blocks": 150000,
  "timeoffset": 0,
  "connections": 8,
  "difficulty": 1024.5,
  "testnet": false,
  "utreexo": true,         // Always true for DineroCoin
  "warnings": ""
}
```

**Version Encoding:**
```cpp
#define CLIENT_VERSION_MAJOR 1
#define CLIENT_VERSION_MINOR 0
#define CLIENT_VERSION_REVISION 0
#define CLIENT_VERSION (1000000 * CLIENT_VERSION_MAJOR + 10000 * CLIENT_VERSION_MINOR + 100 * CLIENT_VERSION_REVISION)
```

---

## Enforcement

### Pre-v1.0.0 (Development)

**Flexibility:** High - Breaking changes allowed
**Testing:** Regtest/Testnet only
**User Expectations:** None - explicitly experimental

### Post-v1.0.0 (Mainnet)

**Flexibility:** Low - Consensus frozen
**Testing:** Mandatory security review for any consensus change
**User Expectations:** Backward compatibility within v1.x

### Post-v2.0.0 (Hard Fork)

**Flexibility:** Medium - New consensus rules established
**Testing:** 180-day testnet minimum
**User Expectations:** Coordinated network upgrade

---

## Appendix A: Quick Reference

### "Can I change this without a version bump?"

| Change | Version Bump | Notes |
|--------|--------------|-------|
| Fix typo in log message | ❌ No | Internal only |
| Add new RPC method | ✅ MINOR | v1.x.0 |
| Fix reorg bug (no consensus change) | ✅ PATCH | v1.x.x |
| Change block validation logic | ✅ MAJOR | v2.0.0 (hard fork) |
| Optimize database queries | ✅ MINOR | v1.x.0 |
| Add Utreexo commitment field | ✅ MAJOR | v2.0.0 (hard fork) |
| Improve P2P bandwidth usage | ✅ MINOR | v1.x.0 |
| Change PoW parameters | ✅ MAJOR | v2.0.0 (hard fork) |

### "What version should this be?"

1. Does it change consensus validation rules? → **MAJOR (v2.0.0)**
2. Does it add features/APIs without breaking old nodes? → **MINOR (v1.x.0)**
3. Does it fix a bug without changing behavior? → **PATCH (v1.x.x)**
4. Is it pre-mainnet development? → **v0.x.x (flexible)**

---

## Appendix B: Comparison to Bitcoin Core

| Aspect | Bitcoin Core | DineroCoin |
|--------|--------------|------------|
| Versioning | Semantic (v0.x → v27.x) | Semantic (v0.x → v1.x → v2.x) |
| Hard Fork Policy | Avoid at all costs | Planned for v2.0+ only |
| Soft Fork Activation | BIP9 (miner signaling) | BIP9-style (75% threshold) |
| Pre-Release Tags | `-rc1`, `-rc2` | `-alpha`, `-beta`, `-rc1` |
| Protocol Version | Separate from client version | Unified in v1.0+ |
| Block Header Size | 80 bytes (fixed since 2009) | 128 bytes (Utreexo + 64-bit timestamp + reserved, fixed at v1.0) |

---

## Document Maintenance

**Authority:** This document is CANONICAL and changes require:
1. Community discussion (GitHub issue)
2. Pull request with rationale
3. Approval from core maintainers
4. Version bump of this document

**Version History:**
- v1.0 (2025-12-13): Initial policy established

---

**Status:** ACTIVE
**Enforcement:** Immediate (v0.9.0+)
**Next Review:** Before v1.0.0 mainnet launch
