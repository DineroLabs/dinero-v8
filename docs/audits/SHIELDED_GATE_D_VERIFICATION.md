# Gate D — snapshot binding verification

This gate tests the snapshot-trust upgrade on `codex/shielded-integration`. Mainnet and testnet activation remain dormant. It is not independent consensus review or activation approval.

## Direct binding tests

`SnapshotBinding` directly calls the branch generator, verifier, binding evaluator and burial evaluator. Six tests cover:

- Independent OpenSSL double-SHA256 Merkle oracle for 1, 2, 3, 5, 6 and 17 transactions, with explicit carried-branch bytes for even and odd trees. A production writer/verifier agreeing with each other is insufficient.
- Empty branch, wrong leaf/sibling, 32-level acceptance and 33-level rejection.
- Exact binding failure classes, including missing/duplicate/malformed DNRS and branch verification before commitment interpretation.
- Nullifier and anchor changes altering full SHR1 while the commitment tree is unchanged.
- Selected-chain ancestry, burial threshold equality and both sides, missing ancestor, and UINT32_MAX arithmetic boundaries.
- Stable diagnostic names.

## Six independent loader mutations

The Python standard-library harness mines a real regtest chain, exports a v5 snapshot with valid Utreexo state, recomputes the checksum after each byte mutation, and invokes the real `loadtxoutset` RPC on a fresh consumer with copied PoW-validated headers. The ancestry case changes the selected header chain while retaining the original snapshot: this tests chain context independently of payload bytes.

| Mutation | Required rejection |
|---|---|
| Shielded anchor payload (tree and Utreexo unchanged) | `commitment-mismatch` |
| Claimed section tree root | `does not match snapshot commitment_root` |
| Coinbase DNRS commitment | `invalid-merkle-proof` (the changed coinbase is not the proven leaf) |
| Merkle branch | `invalid-merkle-proof` |
| Base hash | `not found in chain` (the known-base check precedes body parsing) |
| Selected ancestry | `insufficient-burial-or-non-ancestry` |

The precise class matters; a checksum failure, unrelated refusal or generic failed RPC cannot satisfy the test. Independent direct-helper tests additionally prove that a *proven* malformed/duplicate commitment has its own verdict.

Controls: a valid buried v5 snapshot is accepted with an explicit enforced-verification log; an insufficiently buried base is rejected; v4 is rejected under enforcement and accepted while dormant; a validly parsed forged payload is advisory/accepted while dormant. These eleven cases establish both sides of the activation boundary.

Every rejected load must leave the tip, Utreexo commitment, full shielded root and AssumeUTXO lifecycle unchanged immediately and after restarting the same datadir. These are load-time rejection checks, not claims about eventual replay behavior. The harness has no peers and does not wait for background validation to reject the forgery.

## Real defect found and repaired

Before the fix, the payload case returned `commitment-mismatch` **after** importing state: the consumer's forest changed from 0 to 36 leaves and its shielded root changed despite RPC failure. The test failed on that state difference.

Binding verification now runs against temporary decoded shielded containers before BulkLoad, base-header caching, lifecycle transitions or snapshot metadata writes. The live restore uses the same nullifier decoder, replaces stale content rather than ignoring insertion errors, and checks that the imported root matches the authenticated candidate before persisting shielded state. Consensus root encoding is unchanged.

The guarantee tested here is that invalid snapshot inputs do not mutate live state. This does not claim transaction-wide rollback of arbitrary storage I/O failures after a valid snapshot begins importing.

## Execution and evidence

Final results: **6/6 direct helper tests, 11/11 loader cases, and 6/6 selected CTest suites passed**. The mutation harness took 135.22 seconds; the selected CTest run took 149.92 seconds. Code commit: `3e3349696`. Exact fixture hashes, observed RPC errors, unchanged-state hashes, source/binary hashes and log hashes are recorded in [SHIELDED_GATE_D_RESULTS.json](SHIELDED_GATE_D_RESULTS.json). `SnapshotBindingMutations` is registered unconditionally, has a 600-second cap, runs serially, and is explicitly selected by the mandatory serial CI lane. CI retains failed fixture files and bounded daemon log tails. `SnapshotBinding` is selected by the ordinary unit lane. The CTest integrity checker finds all 574 registered executables.

The full Linux execution-baseline check cannot be credited from this Mac configuration: it reports the macOS-only `MacOSNestedBundleSigning` test and absent Linux `RelayNatNetnsHarness` baseline entry. The per-test execution map confirms both new gates are selected; no unrelated baseline was weakened.

Remaining gates include independent human review, external-miner/coordinator DNRS support, ratification of burial policy, cross-platform release qualification and a separately reviewed production activation decision.
