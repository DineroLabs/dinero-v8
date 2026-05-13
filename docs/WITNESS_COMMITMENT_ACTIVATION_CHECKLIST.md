# Witness Commitment Enforcement Activation Checklist

**Status:** ✅ ACTIVE - Enforcement enabled from block 2 (mainnet/testnet)
**Purpose:** Reference for witness commitment enforcement rules
**Audience:** Protocol maintainers, network operators, miners

---

## Executive Summary

Witness commitment enforcement is **ACTIVE** on Dinero mainnet and testnet from block 2.

**Current State (LIVE):**
- ✅ Enforcement plumbing exists
- ✅ Enforcement is ON for mainnet (from block 2)
- ✅ Enforcement is ON for testnet (from block 2)
- ⚠️ Enforcement is OFF for regtest (configurable for tests)

**Activation Complete:** Blocks with witness data MUST include valid DINW commitment.

---

## Part 1: What Activation Means

### Technical Definition

Activating witness commitment enforcement means setting:
```cpp
enforce_witness_commitment = true
witness_commitment_enforcement_height = <chosen_height>
```

### Consensus Impact

**When `enforce=true` AND `height >= enforcement_height`:**

| Block Type | Commitment Requirement | Consequence |
|------------|----------------------|-------------|
| No witness data | NOT required | Accepted (unchanged) |
| Has witness data | REQUIRED | Rejected if missing/invalid |

**Critical:** This is a **soft fork** if activated on mainnet.

### What Activation Enables

✅ **Guaranteed witness integrity** - Blocks with witness data MUST commit to witness merkle root
✅ **Future segwit readiness** - Infrastructure for witness transaction relay
✅ **Fraud proof foundation** - Witness merkle isolation enables fraud proofs
✅ **Compact block optimization** - Commitment enables efficient block relay

### What Activation Does NOT Enable

❌ **NOT full SegWit activation** - No witness discount, no script versioning
❌ **NOT transaction relay changes** - Witness transactions still relay as-is
❌ **NOT header changes** - Block headers unchanged
❌ **NOT weight/vsize** - Block size limits unchanged
❌ **NOT mandatory for all blocks** - Only blocks with witness data

---

## Part 2: Prerequisites Checklist

### 2.1 Technical Readiness

**Code Quality:**
- [ ] All Phase 11a-11d tests passing (merkle, witness, commitment, enforcement)
- [ ] No regressions in consensus validation
- [ ] Mining creates commitments correctly
- [ ] Validation accepts/rejects commitments correctly
- [ ] Enforcement logic tested across all scenarios

**Infrastructure:**
- [ ] Block explorers can parse DINW commitment format
- [ ] Mining pools understand commitment requirements
- [ ] Wallets handle witness transactions correctly
- [ ] RPC endpoints expose commitment data

**Performance:**
- [ ] Commitment creation adds <10ms to block assembly
- [ ] Commitment validation adds <5ms to block validation
- [ ] No memory leaks in witness merkle computation
- [ ] No performance regressions under load

### 2.2 Network Readiness

**Node Deployment:**
- [ ] >90% of nodes upgraded to Phase 11d+ codebase
- [ ] Mining pools representing >75% hashrate upgraded
- [ ] Major economic nodes (exchanges, merchants) upgraded
- [ ] Sufficient time for stragglers to upgrade (suggested: 3-6 months)

**Testing:**
- [ ] Regtest activation tested end-to-end
- [ ] Testnet activation completed successfully
- [ ] No consensus splits observed on testnet
- [ ] Commitment enforcement stable for >1000 testnet blocks

### 2.3 Ecosystem Coordination

**Mining Pools:**
- [ ] Pools notified of activation timeline (>3 months advance notice)
- [ ] Pools confirm mining software supports commitments
- [ ] Pools tested commitment creation on testnet
- [ ] Pools agree on activation height

**Exchanges:**
- [ ] Major exchanges upgraded to Phase 11d+
- [ ] Exchanges tested deposit/withdrawal with witness transactions
- [ ] Exchanges aware of activation timeline
- [ ] No exchange opposes activation

**Wallet Providers:**
- [ ] Mobile/desktop wallets upgraded
- [ ] Hardware wallets support witness transactions
- [ ] Web wallets handle commitment validation
- [ ] No critical wallet incompatibilities

### 2.4 Governance Consensus

**Community Agreement:**
- [ ] Public proposal published (GitHub issue, forum post, etc.)
- [ ] Community discussion period completed (suggested: 30+ days)
- [ ] No major technical objections unresolved
- [ ] Rough consensus achieved among stakeholders

**Developer Consensus:**
- [ ] Core developers agree activation is safe
- [ ] Independent security review completed
- [ ] No unresolved concerns about implementation

**Economic Consensus:**
- [ ] Mining pools representing >80% hashrate support activation
- [ ] Major exchanges support activation
- [ ] No significant economic opposition

---

## Part 3: Activation Parameters

### 3.1 Choosing Activation Height

**Regtest:**
- Low height OK (testing only)
- Suggested: height 100-200

**Testnet:**
- Allow sufficient testing period
- Suggested: 2-4 weeks after testnet deployment
- Example: current_height + 2016 blocks (~4 days at 2-min blocks)

**Mainnet:**
- CRITICAL: Allow ample preparation time
- Suggested: 6-12 months after announcement
- Example: current_height + 262,800 blocks (1 year at 2-min blocks)

### 3.2 Rollout Strategy

**Recommended Sequence:**

1. **Regtest Activation** (Day 0)
   - Activate immediately for testing
   - Validate enforcement logic works correctly

2. **Testnet Activation** (Month 1-2)
   - Announce activation height
   - Monitor for issues
   - Iterate if bugs found

3. **Mainnet Announcement** (Month 3)
   - Publish activation proposal
   - Specify exact activation height
   - Open community feedback period

4. **Mainnet Preparation** (Month 3-9)
   - Ecosystem upgrades
   - Miner coordination
   - Exchange readiness

5. **Mainnet Activation** (Month 9-12)
   - Height reached
   - Enforcement begins
   - Monitor network health

---

## Part 4: Risk Assessment

### 4.1 Technical Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Consensus split (upgraded vs non-upgraded nodes) | Medium | High | Ensure >95% node upgrade before activation |
| Mining pools create invalid commitments | Low | Medium | Extensive testnet validation, pool coordination |
| Commitment validation bug | Low | High | Comprehensive test suite, security review |
| Performance regression | Low | Medium | Benchmark before activation |

### 4.2 Ecosystem Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Exchanges unprepared at activation | Medium | High | 6+ month notice, direct communication |
| Mining pool opposition | Low | High | Early engagement, clear benefits communication |
| Wallet incompatibilities | Medium | Medium | Wallet developer outreach, testing support |
| User confusion | High | Low | Clear communication, documentation |

### 4.3 Governance Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Community opposition | Low | High | Transparent process, address concerns |
| Contentious activation | Low | High | Rough consensus required before proceeding |
| Rushed timeline | Medium | High | Strict adherence to preparation milestones |

---

## Part 5: Monitoring and Success Criteria

### 5.1 Pre-Activation Monitoring

**Leading Indicators (Monitor 1-3 months before activation):**
- [ ] Node version distribution (target: >95% on Phase 11d+)
- [ ] Mining pool readiness (target: 100% of top 10 pools)
- [ ] Testnet commitment success rate (target: >99.9%)
- [ ] Community sentiment (no major opposition)

### 5.2 Post-Activation Monitoring

**Critical Metrics (Monitor first 1000 blocks after activation):**
- [ ] Consensus split detection (zero tolerance)
- [ ] Invalid commitment rate (target: <0.1%)
- [ ] Orphan block rate (should not increase)
- [ ] Network hashrate (should remain stable)

**Health Indicators:**
- [ ] Average block time unchanged
- [ ] Transaction throughput unchanged
- [ ] Mempool behavior normal
- [ ] No exchange deposit/withdrawal issues

### 5.3 Success Criteria

**Activation is successful if:**
- ✅ No consensus split after 1000 blocks
- ✅ >99% of witness blocks have valid commitments
- ✅ No mining pool creates invalid commitments
- ✅ Network hashrate remains stable (±10%)
- ✅ No exchange halts deposits/withdrawals
- ✅ No emergency rollback required

---

## Part 6: Rollback and Contingency

### 6.1 Emergency Rollback Conditions

**Immediate rollback required if:**
- ❌ Consensus split detected (chain split >6 blocks)
- ❌ >5% of blocks have invalid commitments
- ❌ Network hashrate drops >30%
- ❌ Critical bug discovered in enforcement logic
- ❌ Major exchange halts trading due to activation

### 6.2 Rollback Procedure

**Emergency Deactivation:**
```cpp
// Revert to pre-activation state
enforce_witness_commitment = false
witness_commitment_enforcement_height = UINT32_MAX
```

**Rollback Steps:**
1. Emergency release with enforcement disabled
2. Coordinate with mining pools for rapid deployment
3. Public announcement explaining rollback
4. Post-mortem analysis of failure cause
5. Fix issues before retry

### 6.3 Partial Activation Strategy

**If risks are high, consider:**
- Testnet-only activation (indefinitely)
- Mainnet activation with very high threshold (e.g., height = 10 years)
- Regtest-only for testing purposes

**Remember:** Not activating is a valid choice. Infrastructure exists regardless.

---

## Part 7: Decision Framework

### 7.1 Go/No-Go Decision Tree

```
START: Should we activate witness commitment enforcement?
│
├─ Are all Phase 11d tests passing?
│  ├─ NO → STOP (fix tests first)
│  └─ YES → Continue
│
├─ Do we have rough consensus from stakeholders?
│  ├─ NO → STOP (build consensus first)
│  └─ YES → Continue
│
├─ Are >90% of nodes upgraded to Phase 11d+?
│  ├─ NO → STOP (wait for upgrades)
│  └─ YES → Continue
│
├─ Have we tested on testnet successfully?
│  ├─ NO → STOP (testnet first)
│  └─ YES → Continue
│
├─ Do all mining pools support activation?
│  ├─ NO → STOP (coordinate with pools)
│  └─ YES → Continue
│
├─ Is the ecosystem ready (exchanges, wallets)?
│  ├─ NO → STOP (coordinate with ecosystem)
│  └─ YES → Continue
│
├─ Have we allowed sufficient preparation time (6+ months)?
│  ├─ NO → STOP (extend timeline)
│  └─ YES → GO (proceed with activation)
```

### 7.2 Network-Specific Recommendations

**Regtest:**
- **Default:** Activate immediately for testing
- **Purpose:** Validate enforcement logic
- **Risk:** None (isolated test network)

**Testnet:**
- **Default:** Activate after 1-2 months of Phase 11d deployment
- **Purpose:** Ecosystem testing, bug discovery
- **Risk:** Low (testnet coins have no value)

**Mainnet:**
- **Default:** DO NOT activate without completing full checklist
- **Purpose:** Production enforcement
- **Risk:** High (consensus split, economic impact)
- **Timeline:** Minimum 6-12 months preparation

---

## Part 8: Communication Plan

### 8.1 Pre-Activation Communications

**Announcement (Month 0):**
- GitHub issue: "Proposal: Witness Commitment Enforcement Activation"
- Forum post: Detailed rationale and timeline
- Twitter/social: Public announcement
- Developer mailing list: Technical details

**Progress Updates (Monthly):**
- Node upgrade status
- Mining pool coordination updates
- Testnet activation results
- Community feedback summary

**Final Notice (1 month before):**
- Reminder of activation height
- Final call for ecosystem upgrades
- Monitoring plan details

### 8.2 Post-Activation Communications

**Success Announcement:**
- Network health metrics
- Thank ecosystem participants
- Next steps (if any)

**Issue Communications:**
- Transparent reporting of any problems
- Remediation plan
- Revised timeline (if rollback required)

---

## Part 9: Example Activation Timeline

### Mainnet Activation Example (12-Month Plan)

| Month | Milestone | Stakeholders |
|-------|-----------|--------------|
| 0 | Phase 11d complete, publish activation proposal | Core devs |
| 1-2 | Testnet activation, ecosystem testing | Pools, wallets, exchanges |
| 3 | Mainnet activation height announced | All |
| 3-9 | Ecosystem upgrade period | Exchanges, wallets, nodes |
| 6 | Midpoint checkpoint (>80% nodes upgraded) | Core devs |
| 9 | Final readiness check (>95% nodes upgraded) | All |
| 10-11 | Final preparation, activation imminent | All |
| 12 | Activation height reached, enforcement begins | All |
| 12+ | Post-activation monitoring (1000 blocks) | Core devs |

### Testnet Activation Example (2-Month Plan)

| Week | Milestone |
|------|-----------|
| 0 | Deploy Phase 11d to testnet |
| 2 | Announce activation height (2 weeks out) |
| 4 | Activation height reached |
| 4-8 | Monitor stability, validate commitment behavior |
| 8+ | Testnet stable, ready for mainnet consideration |

---

## Part 10: Final Checklist

Before activating on **any** network, verify:

### Technical
- [ ] All tests passing (merkle, witness, commitment, enforcement)
- [ ] No known bugs in enforcement logic
- [ ] Performance acceptable (<10ms overhead)
- [ ] Testnet validation successful

### Ecosystem
- [ ] Node upgrade rate >90% (mainnet) or >80% (testnet)
- [ ] Mining pools ready (100% of top 10)
- [ ] Exchanges ready (all major exchanges)
- [ ] Wallets ready (all major wallets)

### Governance
- [ ] Public proposal published
- [ ] Community feedback period completed (30+ days)
- [ ] Rough consensus achieved
- [ ] No unresolved objections

### Timeline
- [ ] Sufficient preparation time (6+ months for mainnet)
- [ ] Activation height publicly announced
- [ ] Ecosystem coordinated on timeline

### Monitoring
- [ ] Monitoring infrastructure ready
- [ ] Alert system configured
- [ ] Rollback plan documented
- [ ] Emergency contact list prepared

---

## Part 11: When NOT to Activate

**DO NOT activate if:**

❌ Any critical test is failing
❌ Node upgrade rate <90% (mainnet) or <80% (testnet)
❌ Major mining pool opposition
❌ Known security vulnerability exists
❌ Ecosystem not ready (exchanges, wallets)
❌ Insufficient preparation time
❌ Community opposition exceeds support
❌ Alternative approach is clearly superior
❌ Risk/benefit ratio unfavorable
❌ You have ANY doubt about readiness

**Remember:** Not activating is always an option. The infrastructure exists and is tested. Activation is an explicit choice, not a requirement.

---

## Appendix A: Activation Code Example

**Regtest Activation (Testing Only):**
```cpp
// src/consensus/chainparams_impl.cpp
static ChainParams g_regtest = {
    ...
    .enforce_witness_commitment = true,  // ENABLED for testing
    .witness_commitment_enforcement_height = 100,  // Low height
    ...
};
```

**Mainnet Activation (LIVE):**
```cpp
// src/consensus/chainparams_impl.cpp
static ChainParams g_mainnet = {
    ...
    .enforce_witness_commitment = true,              // ENFORCED on mainnet
    .witness_commitment_enforcement_height = 2,      // First normal block after premine
    ...
};
```

**NOTE:** Enforcement is ACTIVE from block 2. All blocks with witness data must include valid DINW commitment.

---

## Appendix B: Useful Metrics

### Node Version Distribution
```bash
# Query network node versions
dinero-cli getpeerinfo | jq '.[] | .subver' | sort | uniq -c
```

### Mining Pool Readiness
- Poll top 10 mining pools directly
- Check testnet block commitments
- Verify pool software versions

### Commitment Validation Rate
```bash
# On testnet, after activation
dinero-cli getblockchaininfo | jq '.blocks, .headers'
# Check for consensus split (blocks should equal headers)
```

---

## Appendix C: Resources

**Phase 11 Documentation:**
- Phase 11a: Merkle consensus locks
- Phase 11b: Witness merkle isolation
- Phase 11c: Witness commitment (DINW magic)
- Phase 11d: Enforcement plumbing (this document)

**Test Suites:**
- `tests/consensus/test_merkle_invariants.cpp`
- `tests/consensus/test_witness_merkle_isolation.cpp`
- `tests/consensus/test_witness_commitment.cpp`
- `tests/consensus/test_witness_commitment_enforcement.cpp`

**Git Tags:**
- `phase-11a-consensus-lock` - Merkle foundation
- `phase-11b-witness-groundwork` - Witness merkle isolation
- `phase-11c-witness-commitment` - DINW commitment structure
- `phase-11d-enforcement-plumbing` - Enforcement switch (ACTIVE from block 2)

---

## Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-01-17 | 1.0 | Initial activation checklist (Phase 11d complete) |
| 2026-01-21 | 2.0 | Enforcement ACTIVATED from block 2 on mainnet/testnet |

---

**Maintainer Notes:**

This document is a living checklist. Update it as:
- New risks are discovered
- Ecosystem requirements change
- Community feedback is received
- Testnet activation provides new learnings

The goal is to make activation decisions **explicit, informed, and reversible**.

**Final Reminder:** Building the switch ≠ flipping the switch. You have optionality. Use it wisely.
