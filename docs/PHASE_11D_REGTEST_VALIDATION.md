# Witness Commitment Regtest Validation

**Status:** deployed consensus behavior
**Historical name:** Phase 11d
**Updated:** 2026-07-30

This document replaces the pre-activation Phase 11d test plan. That plan
described regtest enforcement as disabled and configurable through daemon
arguments. Neither statement describes the deployed v8 consensus path.

## Authoritative behavior

`ChainParams` is the sole activation source used by `BlockValidator`.
Mainnet, testnet, and regtest all declare:

```cpp
.enforce_witness_commitment = true,
.witness_commitment_enforcement_height = 10670,
```

The rule preserves the validator behavior shipped before this documentation
was corrected:

| Condition | Result |
|---|---|
| A recognized DINW v1 commitment has the wrong hash, at any full-rules height | reject |
| Height is below 10,670 and DINW is absent | accept |
| Height is at least 10,670, any transaction has a serialized witness marker, and DINW is absent | reject |
| Height is at least 10,670 and no transaction has a serialized witness marker | DINW is not required |
| A recognized DINW v1 commitment is valid | accept |

“Has witness” means `Transaction::HasWitness()`:
`witness_version != 0xFF`. It does not mean “an input witness stack is
non-empty.”

The historical-compatibility scan and its counts are recorded in
`WITNESS_COMMITMENT_ACTIVATION_CHECKLIST.md`. In particular, 179 mainnet
blocks before height 10,670 have a witness marker but no DINW output, so the
obsolete declared heights 1 and 2 cannot safely replace the deployed boundary.

## Tests

The focused tests exercise both the helper and the production validator:

```bash
cmake --build build --target \
  test_witness_commitment_enforcement \
  test_block_validation_invariants \
  test_activation_boundary_critical

ctest --test-dir build --output-on-failure \
  -R '^(WitnessCommitmentEnforcement|BlockValidationInvariants|ActivationBoundaryCritical)$'
```

`BlockValidationInvariants` pins:

- acceptance at height 10,669 without DINW;
- rejection at height 10,670 with a witness marker and no DINW;
- acceptance at height 10,670 without a witness marker;
- acceptance of a valid DINW commitment;
- all three networks’ declared boundary; and
- production use of a test-only `MutableParams()` override.

`MutableParams()` is a unit-test facility, not a daemon configuration
interface. Changing an activation value in a shipped binary is a consensus
change and requires historical-compatibility analysis.
