# 🏦 DineroCoin — Exchange Due Diligence Answers

**Version:** v2.0.1-dinero-rings
**Status:** Protocol Core COMPLETE (Rings 1–8 SEALED)
**Audit Anchor:** `09d79aa66fc3f08cc63e68ab0673ff43fdfac88d`

---

## Document Purpose

This document provides standardized answers to cryptocurrency exchange due diligence questions. It is written in exchange-review language to minimize back-and-forth and accelerate listing decisions.

---

## Q1: Is the protocol stable?

**Yes.**

The protocol core is sealed at **v2.0.1-dinero-rings**.

- ✅ No breaking changes allowed
- ✅ No mutable consensus logic
- ✅ Backward compatibility mechanically enforced
- ✅ 100+ properties proven and locked

**Evidence:** 46 test suites, 100% pass rate, mechanically enforced via Ring 8

---

## Q2: Can the rules change unexpectedly?

**No.**

- **Ring 7 semantics are immutable** — Script execution behavior is frozen forever
- **Ring 8 enforces governance in code** — Not policy, but mechanical enforcement
- **Any change must be:**
  - Gated (explicit activation required)
  - Documented (rationale + documentation mandatory)
  - Reviewed (audit trail enforced)
  - Activated (opt-in, never forced)

**Silent forks are impossible by design.**

**Evidence:** See Ring 8 Phase 8a (BC1-BC4), 8b (EG1-EG3), 8c (CL1-CL3)

---

## Q3: Has supply been audited?

**Yes — formally.**

The following properties are **proven** (not just documented):

- ✅ **Supply conservation** — No inflation under any conditions (Ring 1)
- ✅ **No inflation under reorgs** — Supply invariant preserved during chain reorganization (Ring 4)
- ✅ **No inflation under crashes** — Supply invariant preserved across node restarts (Ring 1)
- ✅ **No economic exploits** — Incentive compatibility proven (Ring 6)
- ✅ **No double-spend attacks** — Even under network partitions (Ring 5)

**Total Supply:** 99,000,000 DIN (hard-coded, immutable)

**Evidence:** Ring 1 properties (supply conservation, UTXO consistency), Ring 4 mining properties, Ring 6 economic properties

---

## Q4: How does this compare to Bitcoin?

| Aspect | Bitcoin | DineroCoin |
|--------|---------|------------|
| **Consensus** | Social consensus (BIP process) | Mechanical enforcement (Ring 8) |
| **Rule changes** | Informal (mailing list, BIPs) | Formal (gated, documented, audited) |
| **Backward compatibility** | Best effort | Mechanically guaranteed |
| **Supply audit** | Code review | Formal property verification |
| **Upgrade safety** | Manual testing | Automated property testing |

**Where Bitcoin documents rules, DineroCoin proves them.**

---

## Q5: What about future upgrades?

Upgrades are:

- ✅ **Optional** — Nodes can choose to activate or not
- ✅ **Non-breaking** — Existing behavior never changes
- ✅ **Gated** — Explicit activation required
- ✅ **Auditable** — Full documentation and rationale required
- ✅ **Versioned** — New script versions (VERSION_1+) or namespaces (EXTENSION_1+)

**Exchanges will always know exactly what is activated and when.**

No surprise hard forks.
No forced upgrades.
No semantic drift.

**Evidence:** Ring 8 Phase 8b (Extension Gating & Activation)

---

## Q6: Is there a single audit reference?

**Yes.**

- **Public Release:** v2.0.1-dinero-rings
- **Audit Anchor:** 09d79aa66fc3f08cc63e68ab0673ff43fdfac88d
- **Release URL:** https://github.com/Trucker2827/Dinero-Coin/releases/tag/v2.0.1-dinero-rings

**This will never move.**

All future audits should reference this baseline.
All future changes are additive (extensions), not modifications.

---

## Q7: What is the risk profile?

| Risk Type | Level | Explanation |
|-----------|-------|-------------|
| **Protocol Risk** | 🟢 LOW | Sealed, proven, immutable |
| **Consensus Risk** | 🟢 LOW | Formally verified (Rings 1-7) |
| **Upgrade Risk** | 🟢 LOW | Gated, documented, optional |
| **Governance Risk** | 🟢 LOW | Mechanically enforced (Ring 8) |
| **Supply Risk** | 🟢 LOW | Conservation proven, no inflation possible |
| **Economic Risk** | 🟢 LOW | Incentive compatibility proven (Ring 6) |
| **Network Risk** | 🟢 LOW | Byzantine tolerance proven (Ring 5) |
| **Operational Risk** | 🟡 STANDARD | Normal node operations (same as Bitcoin) |

**Overall Risk Profile: LOW**

This is comparable to Bitcoin's maturity level, but with formal verification guarantees that Bitcoin lacks.

---

## Q8: Why should we list DineroCoin?

Because it offers:

1. **A closed, auditable protocol core** — No moving target
2. **Predictable long-term behavior** — No surprise forks
3. **No governance surprises** — Changes are mechanical, not political
4. **Clear upgrade boundaries** — Always opt-in, never forced
5. **Strong formal guarantees** — 100+ proven properties
6. **Professional audit surface** — Single canonical release (v2.0.1)
7. **Exchange-friendly integration** — See `EXCHANGE_INTEGRATION_GUIDE.md`

**DineroCoin is designed for institutional adoption.**

---

## Q9: What integration resources are available?

### Documentation
- **Exchange Integration Guide:** `EXCHANGE_INTEGRATION_GUIDE.md`
- **RPC API Documentation:** See `docs/rpc/`
- **Auditor Onboarding Pack:** `AUDITOR_ONBOARDING_PACK.md`
- **Comprehensive Release Notes:** `RELEASE_v2.0.1_DRAFT.md`

### Support
- **Exchange Support Email:** exchanges@dinero-coin.com
- **Security Contact:** security@dinero-coin.com
- **Documentation:** https://docs.dinero-coin.com
- **Repository:** https://github.com/Trucker2827/Dinero-Coin

### RPC Compatibility
- **Bitcoin RPC compatible** — Standard commands work (getblockcount, getrawtransaction, etc.)
- **Additional commands** — Confidential transaction support (optional)
- **Standard ports** — P2P: 40999, RPC: 20999

---

## Q10: What about regulatory compliance?

DineroCoin provides:

- ✅ **Transparent blockchain** — All transactions public by default
- ✅ **Optional confidential transactions** — Privacy features (like Monero/Zcash)
- ✅ **Address transparency** — Standard addresses are fully transparent
- ✅ **Audit trail** — Complete transaction history

**Exchanges can choose:**
- **Mode A:** Transparent-only (like Bitcoin) — Easiest compliance
- **Mode B:** Mixed (transparent + confidential) — Advanced privacy

**Recommendation:** Start with Mode A (transparent-only), add Mode B later if desired.

See `EXCHANGE_INTEGRATION_GUIDE.md` for details.

---

## Q11: What is the transaction throughput?

| Metric | Value | Notes |
|--------|-------|-------|
| **Block Time** | 60 seconds | 1 minute blocks |
| **Block Size** | 1 MB | Same as Bitcoin (pre-SegWit) |
| **TPS (typical)** | ~7-10 | Comparable to Bitcoin |
| **TPS (max)** | ~15-20 | With optimized transactions |
| **Confirmation Time** | ~1 minute | First confirmation |
| **Finality** | 6-12 blocks | Recommended for exchanges |

**For large deposits:** 12+ confirmations recommended (same as Bitcoin for large amounts).

---

## Q12: What is the mining algorithm?

DineroCoin uses **CPU-friendly mining** with a phase-based difficulty system:

- **Phase 1 (0-2M DIN):** Developer fund premine
- **Phase 2 (2M-20M DIN):** CPU-friendly mining (100 DIN/block)
- **Phase 3 (20M+ DIN):** Bitcoin-level difficulty with halving

**Current Phase:** Depends on chain height (check `getblockcount`)

**Key Properties:**
- ✅ No ASICs required
- ✅ Fair distribution
- ✅ Proven mining correctness (Ring 4)
- ✅ Proven economic incentives (Ring 6)

---

## Q13: What is the security model?

DineroCoin uses **Proof-of-Work (PoW)** security:

- **Hash Function:** SHA-256 (double hash, same as Bitcoin)
- **Difficulty Adjustment:** Every 60 blocks (~1 hour)
- **51% Attack Resistance:** Same model as Bitcoin
- **Consensus:** Longest chain (Nakamoto consensus)

**Proven Properties:**
- ✅ Mining safety (Ring 4 MS1-MS5)
- ✅ Mining liveness (Ring 4 ML1-ML5)
- ✅ Byzantine tolerance (Ring 5 DB1-DB5)
- ✅ Network partition tolerance (Ring 5 DN1-DN5)

**Security Level:** Comparable to Bitcoin for equivalent hashrate.

---

## Q14: What is the developer fund?

- **Amount:** 2,000,000 DIN (2.02% premine)
- **Type:** P2WPKH (single-signature, standard Bitcoin address format)
- **Purpose:** Protocol development, security audits, ecosystem growth
- **Transparency:** All transactions public on blockchain
- **Governance:** Community oversight, quarterly reports planned

**Total Supply:** 99,000,000 DIN
**Mining Rewards:** 97,000,000 DIN
**Developer Fund:** 2,000,000 DIN

See `scripts/deploy/PREMINE_WORKFLOW.md` for technical details.

---

## Q15: Is there a testnet?

**Regtest available** for testing (private network).

Public testnet launch planned for Q1 2026.

**Current Testing:**
- Run local regtest network: `dinerod -regtest`
- Generate test blocks: `dinero-cli -regtest generate 101`
- Test transactions immediately (no waiting)

See `EXCHANGE_INTEGRATION_GUIDE.md` for testnet setup instructions.

---

## Final Exchange Summary (1 sentence)

**DineroCoin is a Bitcoin-style protocol whose entire consensus, economic, scripting, and governance layers are formally verified, sealed, and enforced by code — not policy.**

---

## Exchange Listing Checklist

Use this checklist for your listing evaluation:

### Technical Due Diligence
- [ ] Protocol stability verified (Q1)
- [ ] Rule change process verified (Q2)
- [ ] Supply audit verified (Q3)
- [ ] Upgrade safety verified (Q5)
- [ ] Audit anchor confirmed (Q6)
- [ ] Risk profile acceptable (Q7)

### Integration Assessment
- [ ] RPC compatibility verified (Q9)
- [ ] Integration guide reviewed
- [ ] Testnet/regtest setup verified
- [ ] Transaction throughput acceptable (Q11)
- [ ] Confirmation times acceptable (Q11)

### Compliance & Security
- [ ] Regulatory compliance approach reviewed (Q10)
- [ ] Security model verified (Q13)
- [ ] Mining algorithm verified (Q12)
- [ ] Developer fund transparency verified (Q14)

### Business Decision
- [ ] Competitive advantages identified (Q8)
- [ ] Support resources confirmed (Q9)
- [ ] Community engagement assessed
- [ ] Listing decision made

---

## Next Steps for Exchange Listing

1. **Review this document** — All standard questions answered
2. **Read integration guide** — See `EXCHANGE_INTEGRATION_GUIDE.md`
3. **Verify protocol** — Run tests (see `AUDITOR_ONBOARDING_PACK.md`)
4. **Test on regtest** — Set up local test environment
5. **Contact exchange support** — exchanges@dinero-coin.com
6. **Negotiate listing terms** — Standard process

---

## Contact Information

### Exchange Inquiries
**Email:** exchanges@dinero-coin.com
**Subject Line:** `[EXCHANGE LISTING] <Your Exchange Name>`
**Response Time:** < 48 hours

### Technical Support
**Email:** security@dinero-coin.com
**PGP:** Available on request

### Public Resources
- **Repository:** https://github.com/Trucker2827/Dinero-Coin
- **Documentation:** https://docs.dinero-coin.com
- **Release:** https://github.com/Trucker2827/Dinero-Coin/releases/tag/v2.0.1-dinero-rings

---

**Document Version:** 1.0
**Last Updated:** January 4, 2026
**Audit Anchor:** v2.0.1-dinero-rings (09d79aa66fc3f08cc63e68ab0673ff43fdfac88d)

---

*This document is intended for cryptocurrency exchange listing teams, compliance officers, and integration engineers evaluating DineroCoin for exchange listing.*
