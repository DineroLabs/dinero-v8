# Successful service-origin capture qualification

`OrchardServiceDeliverySource` now runs `ServiceOriginCaptureChecks` before its existing generated-history refusal cases. This exercises the actual `ChainstateService::getRuntimeWalletOrigin` success path; no source, validation or recovery rule is relaxed.

The isolated fixture starts with the selected regtest genesis and builds three real coinbase blocks. A second, owned `AssumeUtxoReplayEngine` validates the history and supplies its exact coins, forest and legacy shielded state. The fixture derives the zero legacy pool accounting from the selected archival bodies, constructs the required witness/filter/state/forest commitments, validates the boundary header with an owned header selector, and commits the actual first boundary using `PreparedOrchardChainstateWrite::ConnectIndexed`. The wallet-origin source reads the resulting canonical outbox itself. There is no fabricated delivery record or caller-provided first cursor.

The service must return the real height-three parent, exact first source cursor, owned height-one coin and exact relevant transaction. Tests also require:

- The final wallet-domain read occurs and SQL failure there refuses the result.
- A competing thread can acquire the chain lock during both wallet-domain reads.
- A wrong mutable height index does not redirect the hash-bound ancestry.
- A changed archival body refuses a new capture while an already returned projection keeps its original owned data.
- A stale wallet session refuses after reopen, and the new session can capture again.

The existing required root CI lane retains its detailed CTest log and requires the new success marker. Both root inventory and execution selectors remain unchanged at 46 tests. Local qualification executes the affected service-source test, not all 46.

## Scope

This is a short, coinbase-only regtest ancestry with an empty Orchard boundary. Pre-boundary state-commitment enforcement is disabled in this explicit test profile; boundary commitments are required. Regtest uses its existing header/PoW policy. The supported separated storage layout and active service tip are explicitly initialized in the fixture. This does not exercise full daemon startup, production layout migration, mainnet PoW, signed historical spends, CT epochs, nonempty retirement or shielded activity, general history limits, crash recovery or physical power loss.

The success path returns facts for already known ordinary scripts. It does not include the index script domain, complete key/account discovery, local send/pending/orphan-history reconciliation, baseline adoption or receipts. The initial activation still needs its own independently validated selected-parent owner and all configured recovery consumers; a delayed source requiring an existing outbox cannot bootstrap that boundary. Provider installation and user steps 1–4 remain incomplete.

## Local checks

Fresh declared service/replay targets and the full daemon build passed. Three actual CTests passed: `AssumeUtxoReplay`, `OrchardServiceDeliverySource`, and `OrchardServiceStartup`. The default-runtime-reader-off service translation unit also compiled. All 202 linked project C++ translation units in the service executable were freshly instrumented with ASan/UBSan, and the actual origin-capture service path passed. Copied-source controls omitting consensus replay, projection effects or the final wallet-domain check failed their intended assertions; restored source passed. Rust/external libraries remained uninstrumented, macOS leak detection was off, and the separate ARM dependency qualification gate remains open. This is not release binary provenance.
