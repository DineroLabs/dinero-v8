# Ring 6: Economic Execution & Incentives — Design Document

**Status**: DESIGN PHASE (Not Yet Implemented)
**Date**: 2026-01-03
**Precondition**: Ring 5 SEALED (Distributed Consensus Proven)
**Approach**: Oracle-based economic property verification

---

## Executive Summary

Ring 6 proves that DineroCoin's economic layer is correct: fees are validated properly, incentives align with network security, and economic attacks are prevented. This ring extends the observable-facts-only pattern from Ring 5 to economic properties.

**Key Innovation**: Economic oracles that verify fee validation, mempool economics, and incentive alignment without requiring economic prediction or market modeling.

---

## 1. What Ring 6 Proves

### Ring Architecture Hierarchy

```
Ring 1: Cryptographic Primitives (supply, UTXO, chain selection) ✅ SEALED
Ring 2: Consensus Validation (block/tx/script validity) ✅ SEALED
Ring 3: P2P Network (connection management, messaging) ✅ SEALED
Ring 4: Mining (block generation, subsidy, determinism) ✅ SEALED
Ring 5: Distributed Consensus (safety, liveness, Byzantine) ✅ SEALED
Ring 6: Economic Execution (fees, incentives, economics) ← DESIGN
```

### Scope Boundary

**Ring 5 Proved**: Honest nodes agree, network converges under adversarial conditions
**Ring 6 Proves**: Economic layer functions correctly, incentives align with security

**Ring 6 Does NOT Prove**: Market prices, miner profitability, user behavior

---

## 2. Property Categories

Ring 6 follows the same 5-category structure as Ring 5:

### E1-E5: Economic Safety Properties
**Question**: "Can economic rules be violated?"

- **E1: Fee Validation** - Invalid fee transactions never accepted
- **E2: Value Conservation** - Fees properly calculated (output - input ≤ 0)
- **E3: Fee Overflow Protection** - Fee calculations never overflow
- **E4: Minimum Relay Fee** - Below-minimum-fee txs never relayed
- **E5: Dust Threshold** - Dust outputs properly rejected

### E6-E10: Economic Liveness Properties
**Question**: "Do valid economic transactions eventually succeed?"

- **E6: Fee-Bearing TX Inclusion** - Valid fee-bearing txs eventually included
- **E7: Mempool Replacement** - RBF txs eventually replace lower-fee versions
- **E8: Fee Estimation** - Fee estimator provides bounded estimates
- **E9: Block Assembly** - Templates include highest-fee valid transactions
- **E10: Economic Finality** - Transactions with sufficient fee depth don't reorg out

### E11-E15: Incentive Alignment Properties
**Question**: "Do incentives align with network security?"

- **E11: Mining Reward Maximization** - Miners incentivized to include valid high-fee txs
- **E12: No Selfish Mining Advantage** - Selfish mining provides no economic benefit (bridges Ring 5)
- **E13: No Fee Sniping Advantage** - Fee sniping doesn't provide profitable advantage
- **E14: No Pinning Profit** - Transaction pinning doesn't yield economic gain
- **E15: CPFP Effectiveness** - Child-pays-for-parent properly incentivizes inclusion

### E16-E20: Economic Attack Resistance
**Question**: "Can economic attacks succeed?"

- **E16: No Free Relay** - Spam txs incur relay costs
- **E17: Replace-by-Fee Protection** - RBF can't be exploited for free relay
- **E18: Package Relay Integrity** - Package relay can't be abused
- **E19: Fee Bumping Safety** - Fee bumps properly validated
- **E20: Economic DoS Resistance** - Economic attacks don't DoS the mempool

### E21-E25: Economic Determinism Properties
**Question**: "Are economic calculations reproducible?"

- **E21: Fee Calculation Determinism** - Same inputs → same fee calculation
- **E22: Mempool Ordering Determinism** - Same txs → same mempool ordering (given fee policy)
- **E23: Block Template Determinism** - Same mempool → same block template
- **E24: Fee Estimation Determinism** - Same history → same fee estimates
- **E25: Economic State Determinism** - Same actions → same economic state

---

## 3. Observable-Facts-Only Pattern for Economics

### Critical Principle from Ring 5

Ring 5 established: **"Oracles may only assert properties over facts that exist in the trace."**

Ring 6 extends this to economic facts.

### Economic Facts (Observable)

✅ **Transaction Fee** - `fee = sum(inputs) - sum(outputs)` (observable in trace)
✅ **Mempool State** - Which txs are in mempool at time T (observable)
✅ **Block Template** - Which txs selected for mining (observable)
✅ **Fee Rate** - `fee / tx_size` (computable from observable data)
✅ **Replacement Event** - TX replaced another TX (observable event)

### Economic Intent (NOT Observable)

❌ **"Miner should maximize revenue"** - Requires economic intent inference
❌ **"TX is economically rational"** - Requires user intent inference
❌ **"Fee is too high/low"** - Requires market price prediction

### Example Reframing (Like Ring 5 DN1 → DN2)

❌ **WRONG**: "Miners MUST maximize fee revenue"
- Requires inferring miner intent
- Requires defining "must"
- Not observable from trace

✅ **RIGHT**: "Block template includes highest-fee valid txs from mempool"
- Observable: Check template contents vs mempool state
- No inference about miner intent
- Facts-only verification

---

## 4. Ring 6 Architecture

### Core Components

```
EconomicSimulator
├── MempoolSimulator (extends Ring 5 ConsensusNode)
│   ├── FeeCalculator (deterministic fee computation)
│   ├── MempoolPolicy (RBF, CPFP, replacement rules)
│   └── FeeEstimator (historical fee analysis)
├── BlockAssembler (extends Ring 4 MiningSimulator)
│   ├── TemplateBuilder (fee-maximizing selection)
│   ├── PackageEvaluator (CPFP/package relay)
│   └── PriorityQueue (fee-ordered tx selection)
└── EconomicTrace (execution history)
    ├── FeeAction (tx broadcast, replacement)
    ├── FeeEvent (tx accepted, rejected, included)
    └── FeeState (mempool state, fee rates)
```

### Integration with Ring 5

Ring 6 **extends** Ring 5, not replaces it:

```
ConsensusNode (Ring 5)
    ↓ extends
EconomicNode (Ring 6)
    + FeeCalculator
    + MempoolPolicy
    + FeeEstimator
```

**Boundary**: Ring 5 proves consensus correctness. Ring 6 proves economic layer on top of that consensus.

---

## 5. Property Definitions (Detailed)

### E1: Fee Validation (Economic Safety)

**Property**: Invalid fee transactions never accepted

**Observable Definition**:
- Fee = sum(inputs) - sum(outputs)
- If fee < 0 → tx rejected
- If fee causes overflow → tx rejected
- No negative-fee txs in mempool or blocks

**Violation Detection**:
```cpp
for (const auto& tx : trace.accepted_transactions) {
    int64_t fee = calculateFee(tx);
    if (fee < 0) {
        return violation("Negative fee tx accepted");
    }
}
```

**Why Observable**: Fee calculation is deterministic arithmetic on observable values.

---

### E6: Fee-Bearing TX Inclusion (Economic Liveness)

**Property**: Valid fee-bearing txs eventually included in blocks

**Observable Definition**:
- TX has fee >= minimum relay fee
- TX valid according to Ring 2 rules
- TX broadcast at time T
- Check: TX in block by time T + timeout

**Violation Detection**:
```cpp
for (const auto& tx : trace.broadcast_transactions) {
    if (tx.fee >= min_relay_fee && isValid(tx)) {
        if (!includedInBlock(tx, tx.broadcast_time + timeout)) {
            return violation("Valid fee-bearing tx not included");
        }
    }
}
```

**Why Observable**: Inclusion is an observable event (tx in block or not).

---

### E11: Mining Reward Maximization (Incentive Alignment)

**Property**: Block templates include highest-fee valid txs from mempool

**Observable Definition** (Reframed):
- Mempool state at time T: {tx1, tx2, tx3, ...}
- Block template at time T: {selected_tx1, selected_tx2, ...}
- Check: No valid tx in mempool with higher fee rate excluded

**Violation Detection**:
```cpp
auto mempool_txs = getMempoolAtTime(trace, time);
auto template_txs = getBlockTemplate(trace, time);

for (const auto& excluded_tx : setDifference(mempool_txs, template_txs)) {
    if (excluded_tx.fee_rate > minFeeRateInTemplate(template_txs)) {
        return violation("Higher-fee valid tx excluded from template");
    }
}
```

**Why Observable**: Mempool state and template contents are observable facts.

**NOT Checking**: Whether miner "intends" to maximize revenue (intent inference forbidden).

---

### E12: No Selfish Mining Advantage (Bridge to Ring 5)

**Property**: Selfish mining provides no economic benefit over honest mining

**Observable Definition**:
- Run scenario: honest miners vs selfish miners
- Measure: Total fees earned by each group
- Check: Selfish miners earn ≤ honest miners (proportional to hashpower)

**Bridge to Ring 5**: Uses Ring 5's Byzantine tolerance framework (DB1-DB5) but measures economic outcomes.

**Violation Detection**:
```cpp
auto honest_revenue = calculateRevenue(trace, honest_miners);
auto selfish_revenue = calculateRevenue(trace, selfish_miners);

double honest_hashpower_fraction = getHashpowerFraction(honest_miners);
double selfish_hashpower_fraction = getHashpowerFraction(selfish_miners);

if (selfish_revenue / selfish_hashpower_fraction >
    honest_revenue / honest_hashpower_fraction * tolerance) {
    return violation("Selfish mining provides economic advantage");
}
```

**Why Observable**: Revenue and hashpower are observable facts from trace.

---

### E21: Fee Calculation Determinism (Economic Determinism)

**Property**: Same inputs → same fee calculation

**Observable Definition**:
- Run fee calculation N times with same inputs
- Check: All results identical

**Violation Detection**:
```cpp
std::vector<int64_t> fee_results;
for (int i = 0; i < N; ++i) {
    fee_results.push_back(calculateFee(tx, utxo_set));
}

if (!allEqual(fee_results)) {
    return violation("Fee calculation non-deterministic");
}
```

**Why Observable**: Fee calculation is a pure function over observable inputs.

---

## 6. Phasing Strategy

### Phase 6a: Economic Simulator Foundation (Weeks 1-2) - P0

**Goal**: Extend Ring 5 simulator with economic layer

**Deliverables**:
- `FeeCalculator` (deterministic fee computation)
- `MempoolPolicy` (RBF, CPFP, replacement rules)
- `EconomicTrace` (fee actions, events, states)
- Smoke tests (fee calculation, mempool operations)

**Exit Criteria**: Fee calculations deterministic, mempool operations traced

---

### Phase 6b: Economic Safety (E1-E5) (Weeks 3-4) - P0

**Goal**: Prove E1-E5 safety properties

**Properties**:
- E1: Fee Validation
- E2: Value Conservation
- E3: Fee Overflow Protection
- E4: Minimum Relay Fee
- E5: Dust Threshold

**Test Scenarios**: Invalid fees, overflow attempts, dust outputs

---

### Phase 6c: Economic Liveness (E6-E10) (Weeks 5-6) - P0

**Goal**: Prove E6-E10 liveness properties

**Properties**:
- E6: Fee-Bearing TX Inclusion
- E7: Mempool Replacement
- E8: Fee Estimation
- E9: Block Assembly
- E10: Economic Finality

**Test Scenarios**: Fee markets, RBF, CPFP, reorgs

---

### Phase 6d: Incentive Alignment (E11-E15) (Weeks 7-8) - P1

**Goal**: Prove E11-E15 incentive properties

**Properties**:
- E11: Mining Reward Maximization
- E12: No Selfish Mining Advantage (Ring 5 bridge)
- E13: No Fee Sniping Advantage
- E14: No Pinning Profit
- E15: CPFP Effectiveness

**Test Scenarios**: Selfish mining, fee sniping, pinning attacks

---

### Phase 6e: Economic Attack Resistance (E16-E20) (Weeks 9-10) - P1

**Goal**: Prove E16-E20 attack resistance properties

**Properties**:
- E16: No Free Relay
- E17: Replace-by-Fee Protection
- E18: Package Relay Integrity
- E19: Fee Bumping Safety
- E20: Economic DoS Resistance

**Test Scenarios**: Spam attacks, RBF abuse, package relay exploits

---

### Phase 6f: Economic Determinism (E21-E25) (Week 11) - P0

**Goal**: Prove E21-E25 determinism properties

**Properties**:
- E21: Fee Calculation Determinism
- E22: Mempool Ordering Determinism
- E23: Block Template Determinism
- E24: Fee Estimation Determinism
- E25: Economic State Determinism

**Test Scenarios**: Replay scenarios 1000x, verify economic state equality

---

## 7. Test Pattern and Sample Sizes

| Category | Properties | Iterations | Total Tests |
|----------|-----------|-----------|-------------|
| Safety (E1-E5) | 5 | 1000 | 5,000 |
| Liveness (E6-E10) | 5 | 1000 | 5,000 |
| Incentive (E11-E15) | 5 | 500 | 2,500 |
| Attack Resistance (E16-E20) | 5 | 500 | 2,500 |
| Determinism (E21-E25) | 5 | 10,000 | 50,000 |

**Total: 25 properties, 65,000 test iterations** (same as Ring 5)

---

## 8. Ring Boundaries

### Ring 6 ↔ Ring 5 (Below)

**Input from Ring 5**: Consensus correctness (nodes agree, network converges)

**Dependency**: Ring 6 assumes Ring 5 consensus properties hold

**Boundary**:
- Ring 5: "Do nodes agree on blocks?"
- Ring 6: "Do blocks include the right transactions economically?"

**Integration**: `EconomicNode` extends `ConsensusNode` from Ring 5

---

### Ring 6 ↔ Ring 7 (Above)

**Output to Ring 7**: Economic correctness guarantees

**Next Layer**: Ring 7 would prove application-layer properties (smart contracts, L2s, etc.)

**Boundary**:
- Ring 6: "Are economic rules enforced?"
- Ring 7: "Do applications execute correctly?"

---

## 9. Critical Design Decisions

### 9.1 Fee Calculation Model

**Decision**: Deterministic arithmetic over observable UTXO values

**Rationale**:
- Fee = sum(inputs) - sum(outputs)
- UTXO values observable from Ring 2 validation
- No market price modeling required

---

### 9.2 Mempool Simulation

**Decision**: Deterministic mempool policy (RBF, CPFP)

**Rationale**:
- Policy rules are deterministic (fee rate comparison)
- Mempool state is observable
- No miner behavior modeling required

---

### 9.3 Block Template Assembly

**Decision**: Observable template contents vs mempool state

**Rationale**:
- Template = list of selected txs (observable)
- Mempool = list of available txs (observable)
- Check: highest-fee txs selected (no intent inference)

---

### 9.4 Incentive Alignment Verification

**Decision**: Measure economic outcomes, not predict behavior

**Rationale**:
- Revenue = observable fees earned
- Hashpower = observable mining power
- Compare outcomes (no economic prediction)

---

### 9.5 Economic Determinism

**Decision**: Same as Ring 5 - replay with same seed

**Rationale**:
- Economic calculations are pure functions
- Same seed → same mempool → same templates
- Determinism critical for reproducibility

---

## 10. What Ring 6 Does NOT Prove

❌ **Market Prices**: Ring 6 doesn't predict DIN/USD exchange rate
❌ **Miner Profitability**: Ring 6 doesn't model electricity costs
❌ **User Behavior**: Ring 6 doesn't predict which txs users broadcast
❌ **Fee Market Dynamics**: Ring 6 doesn't predict long-term fee trends
❌ **Economic Optimality**: Ring 6 doesn't prove "best" economic design

**Ring 6 proves economic correctness, not economic optimality.**

---

## 11. Exit Criteria for Seal

### Functional Completeness
- [ ] All 25 properties implemented (E1-E25)
- [ ] All 25 oracles pass on valid scenarios
- [ ] All 25 oracles detect violations on broken scenarios
- [ ] Smoke tests pass (fee calculator, mempool, assembly)

### Statistical Rigor
- [ ] 1000+ iterations per safety/liveness property
- [ ] 500+ iterations per incentive/attack property
- [ ] 10,000+ iterations per determinism property
- [ ] Total: 65,000+ test iterations in CI

### Determinism Guarantee
- [ ] 100% economic state reproducibility
- [ ] Same seed → same fee calculations
- [ ] Same seed → same mempool state
- [ ] Zero hash collisions

### Zero Flakiness
- [ ] 10 consecutive CI runs with 100% pass rate
- [ ] No random failures in 1 week of CI
- [ ] No timeout-dependent failures

### Coverage
- [ ] All fee scenarios tested (valid, invalid, overflow)
- [ ] All mempool policies tested (RBF, CPFP, replacement)
- [ ] All attack vectors tested (spam, pinning, sniping)
- [ ] All edge cases covered

### Documentation
- [ ] Property specifications documented
- [ ] Oracle detection strategies documented
- [ ] Simulator architecture documented
- [ ] Test patterns documented

### Integration
- [ ] CTest integration complete
- [ ] CI pipeline integration
- [ ] Nightly runs configured
- [ ] Failure notifications working

---

## 12. File Structure (Planned)

```
tests/economic/
├── framework/                          # Simulator infrastructure
│   ├── fee_calculator.h/.cpp          # Deterministic fee computation
│   ├── mempool_simulator.h/.cpp       # Mempool policy simulation
│   ├── block_assembler.h/.cpp         # Fee-maximizing assembly
│   ├── fee_estimator.h/.cpp           # Historical fee analysis
│   ├── economic_trace.h               # Trace structure
│   └── economic_types.h               # Actions/events/states
│
├── properties/                         # Oracle implementations
│   ├── economic_safety_oracle.h/.cpp       # Base E1-E5
│   ├── economic_liveness_oracle.h/.cpp     # Base E6-E10
│   ├── incentive_alignment_oracle.h/.cpp   # Base E11-E15
│   ├── economic_attack_oracle.h/.cpp       # Base E16-E20
│   ├── economic_determinism_oracle.h/.cpp  # Base E21-E25
│   ├── economic_safety_oracle_e1.h/.cpp    # E1: Fee Validation
│   ├── ...                                 # E2-E25
│   └── economic_determinism_oracle_e25.h/.cpp # E25
│
└── tests/                              # Property tests
    ├── test_economic_safety_e1.cpp         # E1 test
    ├── ...                                 # E2-E25
    ├── test_economic_determinism_e25.cpp   # E25 test
    └── test_economic_simulator_smoke.cpp   # Smoke test
```

**Total**: ~80 files (same structure as Ring 5)

---

## 13. Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Economic complexity | Start with simple fee validation (Phase 6b) |
| Non-determinism in fees | Reuse Ring 5's DeterministicScheduler |
| Mempool policy ambiguity | Define observable mempool states |
| Incentive modeling difficulty | Measure outcomes, not predict behavior |
| Economic oracle divergence | Observable-facts-only pattern |

---

## 14. Success Metrics

**Ring 6 is successful when**:

✅ All 25 economic properties proven (E1-E25)
✅ 65,000+ test iterations passing
✅ 100% deterministic economic execution
✅ Zero flakiness (same as Ring 5)
✅ Observable-facts-only pattern maintained
✅ Economic layer correctness guaranteed

---

## 15. Open Questions (Design Phase)

### Q1: Fee Estimator Scope

**Question**: Should Ring 6 prove fee estimator correctness or just test its existence?

**Options**:
1. Prove estimates are bounded (E8: Fee Estimation)
2. Prove estimates converge (deeper property)
3. Just test that estimator runs (minimal)

**Recommendation**: Option 1 (bounded estimates) - observable without prediction

---

### Q2: RBF Policy Completeness

**Question**: Which RBF rules should Ring 6 validate?

**Options**:
1. BIP 125 full compliance
2. Core RBF rules (fee increase, no new unconfirmed parents)
3. Minimal (replacement happens)

**Recommendation**: Option 2 - core rules without BIP 125 edge cases

---

### Q3: CPFP Scope

**Question**: How deep should CPFP package evaluation go?

**Options**:
1. Single parent-child pairs
2. Arbitrary-depth packages
3. Package relay protocol (future)

**Recommendation**: Option 1 for Phase 6c, Option 2 for completeness

---

### Q4: Economic Attack Catalog

**Question**: Which economic attacks should E16-E20 cover?

**Candidates**:
- Free relay spam
- RBF pinning
- Package relay abuse
- Fee bumping exploits
- Mempool DoS

**Recommendation**: Start with top 5, expand if needed

---

### Q5: Ring 5 Integration Strategy

**Question**: How tightly should Ring 6 integrate with Ring 5?

**Options**:
1. Extend ConsensusNode directly
2. Separate EconomicNode that wraps ConsensusNode
3. Parallel simulator

**Recommendation**: Option 1 - extend ConsensusNode (tight integration)

---

## 16. Next Steps (After Design Approval)

### Immediate Actions (Do NOT Implement Yet)

1. **Review this design** with stakeholders
2. **Resolve open questions** (Q1-Q5)
3. **Refine property definitions** if needed
4. **Get design approval** before any code

### Once Approved

1. Start Phase 6a (Economic Simulator Foundation)
2. Create `tests/economic/` directory structure
3. Implement `FeeCalculator` and `MempoolSimulator`
4. Write smoke tests

### Timeline

**Design Phase**: 1 week (current)
**Implementation**: 11 weeks (Phases 6a-6f)
**Total**: 12 weeks (~3 months)

**Minimum Viable Ring 6**: 4 weeks (Phases 6a + 6b)

---

## 17. Conclusion

Ring 6 extends the observable-facts-only pattern from Ring 5 to economic properties. By measuring economic outcomes instead of predicting economic behavior, Ring 6 maintains the determinism and reproducibility guarantees that enabled Rings 1-5 to be sealed.

**Critical Insight**: Economic correctness ≠ economic optimality. Ring 6 proves the economic layer works as specified, not that it's the "best" economic design.

**Status**: DESIGN PHASE - Ready for review and approval

---

**Document Version**: 1.0 (Design)
**Date**: 2026-01-03
**Next Review**: Design approval meeting
**Maintained By**: DineroCoin Engineering Team
