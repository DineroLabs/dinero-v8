# Ring 2 — Consensus Validation Rules

**Status**: 📐 DESIGN
**Date**: 2026-01-03
**Precondition**: Ring 1 SEALED (primitives proven)
**Implementation**: ⏳ PENDING

---

## Purpose

Ring 2 proves that **local consensus validation is mathematically correct**.

This means:
- Invalid blocks are always rejected
- Invalid transactions are always rejected
- Valid blocks/transactions are never rejected
- Consensus rules cannot be bypassed
- State transitions preserve invariants

---

## Scope Boundary

### Ring 2 IS:
- Block structure validation
- Transaction validity rules
- Script execution correctness
- Merkle tree validation
- Coinbase rules enforcement
- Difficulty validation
- Timestamp validation
- State transition correctness

### Ring 2 IS NOT:
- Network propagation (Ring 5)
- Fork choice under delays (Ring 5)
- Mining (Ring 4)
- Persistence (Ring 4)
- P2P threading (Ring 3)

---

## Ring 2 Property Families

### V1 — Block Validity Properties

**Scope**: Structural block validation (stateless checks)

Properties:
- V1.1: Valid blocks must pass validation
- V1.2: Invalid merkle root → rejection
- V1.3: Empty blocks → rejection
- V1.4: Duplicate transactions → rejection
- V1.5: Invalid coinbase → rejection
- V1.6: Malformed header → rejection
- V1.7: Invalid difficulty → rejection

**Test strategy**: Property-based testing with random valid/invalid blocks

---

### V2 — Transaction Validity Properties

**Scope**: Transaction validation (stateless + stateful)

Properties:
- V2.1: Valid transactions must pass validation
- V2.2: Spending non-existent UTXO → rejection
- V2.3: Double-spend within tx → rejection
- V2.4: Negative output value → rejection
- V2.5: Output value > input value → rejection
- V2.6: Invalid signature → rejection
- V2.7: Locktime violation → rejection

**Test strategy**: Property-based testing with random valid/invalid transactions

---

### V3 — Script Execution Properties

**Scope**: Script interpreter correctness

Properties:
- V3.1: Valid scripts evaluate to true
- V3.2: Invalid scripts evaluate to false
- V3.3: Script limits enforced (stack depth, op count)
- V3.4: Disabled opcodes → rejection
- V3.5: P2PKH standard script correctness
- V3.6: P2SH script correctness
- V3.7: Signature verification correctness

**Test strategy**: Property-based testing with script fuzzing

---

### V4 — State Transition Properties

**Scope**: UTXO set updates are correct

Properties:
- V4.1: Applying valid block creates correct UTXO set
- V4.2: Inputs are removed from UTXO set
- V4.3: Outputs are added to UTXO set
- V4.4: Value is conserved (inputs ≥ outputs + fee)
- V4.5: Reorg correctly reverts UTXO set
- V4.6: Coinbase maturity enforced in UTXO queries
- V4.7: No UTXO duplication after apply/revert cycles

**Test strategy**: Property-based testing with random block sequences + reorgs

---

### V5 — Consensus Enforcement Properties

**Scope**: Rules cannot be bypassed

Properties:
- V5.1: Weakened validation → test failure (tripwires)
- V5.2: Consensus params cannot be ignored
- V5.3: Hardcoded checkpoints enforced
- V5.4: Network upgrade activation enforced
- V5.5: AssumeValid does not skip signature checks (only script exec)
- V5.6: All validation paths converge to same result
- V5.7: No validation race conditions

**Test strategy**: Negative testing + tripwire tests

---

## Implementation Strategy

### Phase 2a: Property Test Framework

Create `ConsensusPropertyOracle` base class:
```cpp
class ConsensusPropertyOracle {
public:
    virtual std::vector<ValidationViolation> check(
        const Block& block,
        const ConsensusParams& params,
        const IUTXOSnapshot& utxo_view
    ) const = 0;
};
```

### Phase 2b: V1-V5 Oracle Implementations

Implement 5 oracle classes:
- `BlockValidityOracle` (V1)
- `TransactionValidityOracle` (V2)
- `ScriptExecutionOracle` (V3)
- `StateTransitionOracle` (V4)
- `ConsensusEnforcementOracle` (V5)

### Phase 2c: Property Tests

Run property tests:
- 1,000+ random valid blocks (all V1 properties must pass)
- 1,000+ random invalid blocks (rejection properties must pass)
- 1,000+ random valid/invalid transactions (V2)
- 1,000+ random scripts (V3)
- 100+ random block sequences with reorgs (V4)
- Negative tests for V5 (tripwires)

---

## Exit Criteria

Ring 2 is complete when:

✅ All V1-V5 property tests implemented
✅ All property tests passing (100%)
✅ 10,000+ random test cases executed
✅ No test skips
✅ No flakiness
✅ Tripwires protect against weakened validation

---

## Relationship to Existing Tests

**Existing tests** (keep as-is):
- `test_consensus_validation_tripwires.cpp` - V5 tripwires
- `test_consensus_validation.cpp` - Unit tests for specific cases

**New Ring 2 tests** (property-based):
- `test_ring2_block_validity.cpp` - V1 properties
- `test_ring2_transaction_validity.cpp` - V2 properties
- `test_ring2_script_execution.cpp` - V3 properties
- `test_ring2_state_transitions.cpp` - V4 properties
- `test_ring2_consensus_enforcement.cpp` - V5 properties

Ring 2 property tests **complement** existing unit tests by proving invariants hold across **all possible inputs**, not just specific test cases.

---

## Guarantees After Ring 2 Seal

Once Ring 2 is sealed, we can assume:

| Invariant | Guaranteed |
|-----------|-----------|
| Invalid blocks always rejected | ✅ |
| Invalid transactions always rejected | ✅ |
| Valid blocks/transactions never rejected | ✅ |
| UTXO transitions correct | ✅ |
| Consensus rules enforced | ✅ |
| Script execution deterministic | ✅ |

**These become axioms for Ring 5 (distributed consensus).**

---

## Ring 2 Status

📐 **DESIGN COMPLETE — READY FOR IMPLEMENTATION**

Next: Implement V1-V5 property oracles and tests.
