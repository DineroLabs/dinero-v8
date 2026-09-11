# Dinero v8.1.12 — release candidate notes

This release prepares the network for shielded Auth activation at height **110000**,
DNRS state commitments at **111000**, and contextual transaction-lock enforcement at
**111000**. Operators and miners must upgrade before the relevant activation.

## Changes

- Shielded recipient authority and mobile proving APIs, private covenant wallet/Qt
  flows, and restart/reorg recovery corrections.
- DNRS commitments and snapshot binding/activation safeguards. The first production
  snapshot demonstration with base 111000 requires burial through height 111288.
- Contextual absolute and relative transaction locks in admission, mining, validation
  and replay; wallet funding requires explicit activation capability.
- Candidate ordering and child/parent-link repairs addressing reproducible convergence
  defects, and QUIC shutdown synchronization repair. Historical intermittent issues
  are not all proven closed by these changes.
- Qt covenant navigation, embedded miner/Core identity, pool fee/share presentation,
  paginated lifetime earnings and declared status-schema compatibility.

## Packaging and qualification

The required 45-asset lineup is recorded in `release-assets-v8.1.12.json`, matching
v8.1.11 names with the new version. Preserve Intel Ventura Qt 6.5.3, standard Intel
and ARM desktop packages, Apple signing/notarization and Windows signing.
DineroDPI application integration targets Apple ARM only.

These are candidate notes, not release approval. Publication remains held for final
artifact validation, signing/notarization, supported-device transaction/recovery
qualification, and unresolved review/assurance gates. The independent cryptographic
review package does not substitute for a reviewer completing the review. DineroUS
remains excluded from the initial validator rollout. Production snapshot bootstrap
and background replay must be demonstrated after activation and burial.
