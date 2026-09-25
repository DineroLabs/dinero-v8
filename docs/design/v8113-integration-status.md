# v8.1.13 integration status

This branch collects release implementation for review and qualification against
`dinero-main`. It is a draft candidate, not a release or an activation decision.

## Included source

- Pinned Orchard backend, immutable C++ signing context and bounded draft outer
  transaction envelope, with exact-source component and root-build CI jobs.
- Typed shared reader with historical-format regression tests and a default
  legacy-parser rejection boundary for marked Orchard envelopes.
- Owned consensus-view coin resolution and staged transparent authorization:
  restricted native Taproot/P2WPKH signatures bound to Orchard intent, maturity
  and contextual locks. No production admission caller is enabled.
- Empty-scriptSig envelope rule, host-aligned 100,000-byte ceiling and a shared
  outer/inner signing-profile identity.
- Explicit domain/profile checks across Rust/C++, canonical synthetic vectors,
  transaction identity and authorization-binding tests.
- Shielded database compatibility guard for stopped-copy migration.
- Independent mandatory shielded proof/codec test registrations and CI checks.

These source changes share the public dinero-main history. Separate
historical branch results do not qualify their combination; this candidate needs
its own full Linux build and test runs.

## Still required before release

- Connect the typed shared reader to validated mempool, relay, storage and
  block assembly; current production callers still reject Orchard.
- Connect the staged coin/signature/spendability components to authenticated
  runtime state; qualify combined transparent and Orchard authorization,
  anchors, nullifiers, monetary conservation and activation.
- Atomic Orchard frontier/pool/nullifier updates with UTXOs, tip and undo;
  disconnect, restart, reindex, crash recovery and cross-boundary reorg tests.
- Wallet keys/addresses, proof construction, witness maintenance, shield/send/
  unshield and durable interrupted-operation recovery.
- Complete old/new compatibility, platform, loaded-node and combined lifecycle
  qualification, including production binary provenance.

The new pool starts empty under the owner's release-scope decision. Historical
validation, transparent funds and consistent retired-value accounting remain
requirements. Mainnet activation is unset; this branch deploys nothing. Enabling
the staged backend build option does not activate Orchard transactions.

Security-review materials are maintained separately. This source branch carries
implementation and appropriate regression tests, not the entire review archive.
