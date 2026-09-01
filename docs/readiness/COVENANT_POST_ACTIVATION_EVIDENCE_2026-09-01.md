# Covenant Post-Activation Evidence — 2026-09-01

This record documents the first reviewed mainnet descriptor-funded CTV
lifecycle after covenant activation at height 100,000. No mnemonic, private
key, RPC credential, or wallet-encryption material is included.

## Preconditions

- Local, SJ, NA, and EU1 reported the same mainnet height and block hash before
  funding.
- The wallet was locked while the descriptor and committed transaction were
  reviewed.
- An independent node reproduced the descriptor ID, template hash, descriptor,
  covenant script, destination, amount, version, sequence, and locktime.
- The wallet was unlocked only for the reviewed funding operation and locked
  immediately afterward.

## Reviewed descriptor

- Profile: `ctv`
- Descriptor ID: `48f54f374ae68176f701d5d34599db390c2a77d9cf95a1da963294d96dd6c8d1`
- Template hash: `2636bf80b6339e3ece46e0d86354eae3a020a61a3d215165add4587e524dfe04`
- Committed output: 99,000 una
- Transaction version: 2
- Input sequence: `0xfffffffe`
- Locktime: 0

## Mainnet lifecycle

- Funding transaction:
  `cb97a4fa689b0cea4bbf42704de99834a01cced7b31397ce80bfda7db7d07978`
- Funding amount: 100,000 una
- Funding network fee: 154 una
- Funding confirmation height: 100,017
- Funding block:
  `0000003da6ea90891d2e6a6666a0456752d8ec82c0152e6ee4ad6e16266c4c14`
- Committed spend transaction:
  `0488d5f4f9afa9cd33c060026242931af8467b2881d7b48b82b88f982a8356da`
- Spend confirmation height: 100,021
- Spend output: 99,000 una to the reviewed Taproot destination
- Implied covenant spend fee: 1,000 una
- Observed confirmation count during final verification: 4

The funding outpoint was absent from the UTXO set after confirmation, and the
committed spend output was present with the exact reviewed value and script.

## Persistence and recovery

- After a graceful Qt/daemon restart, the wallet remained locked.
- The descriptor list contained exactly the reviewed descriptor.
- Reimporting the exact descriptor was idempotent and did not create a second
  record.
- A descriptor with a damaged checksum was rejected by inspect/import, and the
  stored descriptor count remained unchanged.

## Mobile status

- Android and iOS covenant navigation and empty/read-only states were exercised
  on their available device/simulator targets.
- Mobile fund movement remains intentionally disabled until the descriptor-
  funded CTV workflow and signing journal are exposed through the embedded
  NodeCore ABI and receive equivalent authorization/reconciliation coverage.

## Local assurance

`./scripts/covenant-readiness.sh --build-dir build-qt-release-ui` passed:

- Covenant architecture and policy boundaries.
- 16 of 16 covenant-labelled tests, including activation, script-path,
  descriptor recovery, multinode relay, restart, and reorg coverage.
- Near-limit resource gate:
  - CTV precomputed validation: 756.5 microseconds per transaction.
  - Taproot precomputed sighash: 2,457 microseconds per transaction.
  - CCV transition: 56.1 microseconds.

## Admission dry-run defect found during smoke test

The smoke test revealed that `mempool.testmempoolaccept` admitted an accepted
transaction into the local mempool instead of remaining read-only. The fix
routes dry-run requests through canonical validation and policy checks but
returns before conflict removal, mempool indexing, UTXO-overlay mutation,
counters, or relay. A regression test verifies two repeated dry-runs leave the
mempool empty and the source UTXO available, followed by successful normal
submission of the same transaction.
