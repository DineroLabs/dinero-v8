# Witness Commitment Consensus Checklist

**Status:** active from height 10,670 on every network
**Purpose:** maintenance checklist for DINW witness-commitment consensus

This file replaces an obsolete pre-activation checklist that named heights 1
and 2, described regtest as disabled, and proposed runtime rollback. The
deployed v8 validator instead used a hardcoded height of 10,670 on every chain.
That behavior is now represented by `ChainParams` and consumed directly by
`BlockValidator`.

## Current rule

At every full-rules height, a recognized DINW v1 commitment must have the
correct hash.

At or above the selected network’s
`witness_commitment_enforcement_height`, a block containing any transaction
for which `Transaction::HasWitness()` is true must contain a valid DINW
commitment in its coinbase.

Current parameters:

| Network | Enabled | Mandatory from |
|---|---:|---:|
| mainnet | yes | 10,670 |
| testnet | yes | 10,670 |
| regtest | yes | 10,670 |

The block assembler may emit DINW before the mandatory boundary. Miner policy
does not redefine the consensus activation height.

## Mainnet history evidence

A read-only scan of the active mainnet chain from genesis through height
10,669 classified every coinbase and, for blocks without a recognized DINW v1
output, inspected the raw transaction serialization:

| Observation | Count / range |
|---|---:|
| Blocks scanned | 10,670 |
| Non-genesis blocks with DINW v1 | 10,490 |
| Blocks without DINW v1 | 179 |
| Missing-DINW height range | 6,485–7,614 |
| Missing-DINW blocks with a serialized witness marker | 179 |
| Missing-DINW blocks with more than one transaction | 0 |
| Missing-DINW blocks from 7,615 through 10,669 | 0 |

Consequently, replacing the deployed 10,670 boundary with the stale declared
heights 1 or 2 would invalidate 179 blocks in active mainnet history. Setting
all three networks to 10,670 is a representation of the rule already executed
by `BlockValidator`, not a new activation.

## Required checks before any future change

1. State the intended validity change for every network.
2. Scan each affected active history for blocks whose acceptance would change.
3. Add boundary tests through the real `BlockValidator` path, not only the
   `EnforceWitnessCommitment()` helper.
4. Preserve validation of recognized DINW v1 commitments before the mandatory
   boundary.
5. Use `Transaction::HasWitness()` as the witness predicate unless a separately
   specified and activated serialization change is intended.
6. Run focused consensus tests, a neuter test that proves the new assertion can
   fail, and the broader consensus suite.
7. Obtain consensus review before merge.

## No runtime rollback

There is no safe operator switch that disables an already-active consensus
rule. Changing `enforce_witness_commitment` or its height in a release can make
nodes disagree about block validity. Emergency handling therefore requires a
reviewed consensus release and coordinated deployment; editing a config file
is not a rollback procedure.

## Focused verification

```bash
cmake --build build --target \
  test_witness_commitment_enforcement \
  test_block_validation_invariants \
  test_activation_boundary_critical

ctest --test-dir build --output-on-failure \
  -R '^(WitnessCommitmentEnforcement|BlockValidationInvariants|ActivationBoundaryCritical)$'
```

The production boundary and parameter-consumption assertions live in
`tests/consensus/test_block_validation_invariants.cpp`.
