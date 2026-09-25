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
- Combined transparent and Orchard authorization for one owned transaction and
  coin snapshot, with honest synthetic shield and cross-address spend fixtures.
  This verifies authorization, not anchor membership, unspentness at application
  time, or an atomic chainstate transition.
- ChainDB staging for Orchard block state, nullifier ownership, anchor references
  and undo, tested with companion coin/tip writes on generated RocksDB stores.
  Production ConnectTip/DisconnectTip callers are not wired yet.
- Independent transparent-value pool arithmetic enforced by the storage staging
  API, starting at zero, including fees, checked bounds and undo. Runtime block
  flow collection is still required; see the pool-guard design document.
- Immutable upstream Orchard commitment frontier with canonical bounded storage
  encoding, derived root/size and failure-atomic append.
- Sealed Orchard state preparation binds parent frontier/root/size, authorization
  context, selected-history anchors, nullifier freshness and ordered pool flows.
  A ChainDB adapter stages that result in the caller's batch and distinguishes
  local corruption/read errors from transaction rejection. Real frontier funding,
  spend, reopen, undo and branch replacement pass in temporary stores. This is
  still not wired into the daemon's mixed-transaction block connector.
- Typed mixed-block candidate reader with shared transaction/witness Merkle and
  DINW checks and base/weight accounting enforced before coin lookup. The ChainDB staging adapter requires exact, ordered authorization
  coverage of every Orchard transaction in that candidate and rejects retired
  legacy shielded transactions. This remains a staged format, not live admission.
- Ordered shared coin processing verifies Orchard and ordinary signatures,
  supports same-block children, prevents cross-family double spending and binds
  the coinbase limit to the same validated fees. Stateful ChainDB adapters stage
  coins, conventional undo and Orchard state together and reverse them after
  checking exact body/undo coverage. Synthetic connect/reopen/disconnect/reconnect
  tests pass; production service integration remains unfinished.
- Stateful mixed forest computation uses the actual transaction identities and
  creation-height leaf rules, excludes transient outputs, checks parent/header
  commitments and supports checked delta rollback on a private clone. A combined
  stateful adapter stages durable delta/checkpoint, forest/height/tip markers and
  coins/Orchard state in one batch; persistent-delta undo survives database reopen.
  Peer proof targets, paths and ordered metadata are checked against resolved
  inputs and the authenticated full parent forest.
  Exact body storage and active transaction indexes now share that batch.
  A versioned commit record shares the batch; tip-local startup auditing and eight process-exit boundaries pass on generated stores. Production/CSN integration and service/flatfile-index coordination remain unfinished.
- Wallet primitives now include opaque ZIP32 account keys, external/internal receivers, watch-only parity and an explicit-network Bech32m address profile. Fresh shield/send/unshield bundle construction uses OS randomness and an owned transaction signing context, then decodes and verifies before returning. Received notes are opaque; incremental witnesses check roots and preserve prior state. No wallet database or RPC caller is enabled.
- Pinned dependency advisory CI gate with saved reports and visible maintenance
  warnings; known vulnerabilities, unsoundness and yanks fail the gate.
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
  runtime state; qualify anchors, nullifiers, pool conservation and activation.
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

## Delivery status

Most release integration remains unimplemented. Component checks do not reduce
the following rows to test-only work:

| Area | Current state |
| --- | --- |
| Shared parsing and authorization | Staged transaction/block readers and exact candidate-bound authorization coverage; no live admission |
| Mempool, relay, block assembly and acceptance | Orchard integration not implemented |
| Anchors, nullifiers, pool and atomic storage/undo | Ordered mixed coin/fee validation, ChainDB coin/state/undo staging and private-clone forest transitions implemented; durable forest/tip/height staging implemented; body/active transaction indexes and versioned commit record staged together; tip-local audit and process-exit boundaries tested; production callers not integrated |
| Wallet keys, addresses, proving, shield/send/unshield | Staged ZIP32 keys/receivers, note reception, incremental witnesses and fresh shield/send/unshield proofs; durable storage, operations and live wallet integration not implemented |
| Restart, reindex, reorg, crash, platform and loaded-node qualification | Orchard end-to-end qualification not started |

The next state integration must use the authoritative ChainDB write batch for
Orchard state, coins, tip and undo together. An isolated successful proof test
or a separate Orchard database commit cannot satisfy that requirement.
