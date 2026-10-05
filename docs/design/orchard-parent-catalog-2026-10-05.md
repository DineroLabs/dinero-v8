# Prepared Orchard parent catalog

## Purpose and owner

Compact validation cannot obtain historical transaction membership from an optional transaction index, or trust legacy coin creation metadata supplied by a peer. The completed independent selected-parent replay now prepares durable content-addressed transaction membership and legacy coin metadata. The service owns both the replay and the exact hash-linked history capture; both must finish before preparation succeeds.

`PreparedOrchardCatalog` has no public factory accepting roots, rows, snapshots, or readiness flags. The service owns the prepared object, its database lifetime and the selected-parent replay. The object remains thread-affine and checks the active profile, genesis, branch and actual maturity-leaf rule. It retains a serialization of the real verification stump derived from the proven forest.

## Storage and bounded preparation

The two Patricia trees use distinct node kinds and checked encodings. Every referenced node is retrieved by its SHA-256 digest and checked for exact framing, kind, child identities and key routing. Traversal has at most 257 nodes. Missing referenced storage is an error; only a checked nonmatching leaf or empty root proves absence. Duplicate insertion refuses.

Transaction leaves retain every captured historical transaction ID, including transactions whose outputs are fully spent. Legacy leaves bind the complete outpoint to its independently replayed creation height and coinbase flag. Coins created at or after the real maturity-leaf activation are not inserted in that legacy tree.

Preparation writes immutable nodes in synchronous batches of at most 1,024 pending nodes. It then reads back all transaction membership and the exact legacy metadata against the completed owners. Typed ChainDB reads and staging validate the node hash and encoding; the ordinary metadata API reserves this namespace. The producer does not rewrite inconsistent existing nodes.

## Publication boundary

The prepared roots remain private and unreachable from canonical state. Preparation does not publish a canonical head, best tip, activation state or readiness certificate. Interrupted or abandoned preparation can leave immutable unreachable nodes. Those nodes cannot authorize compact ingress on their own. Reopen tests only establish structural durability; a new completed owner is still required.

This change does not implement canonical catalog connect/disconnect, undo, restart authority, reindex, compact candidate authentication or a compact canonical writer. The existing stateless Orchard ingress refusal remains. Canonical roots and the full verification stump must eventually commit with the existing Orchard, retirement, body, undo, index, filter, journal, tips and outbox obligations. No second tip or separately committed readiness record is acceptable.

## Qualification scope

Six deterministic cases cover actual service preparation and reopen; the legacy/v2 boundary; incomplete owners, denied history and unavailable storage; interrupted traversal leaving only unreachable immutable nodes; referenced-node corruption without replacement; checked storage and cross-kind reads; and maximum-depth persistent insertion with old-root preservation and duplicate refusal. A backend-disabled case retains refusal. Existing service and replay cases also run.

These small deterministic profiles are not a new high-height proof-of-work, full-node, process-restart, CSN, platform or release qualification. Compiler maps, actual executed test counts, instrumentation boundaries and copied data-validation controls are recorded separately after their terminal results are verified. Mainnet activation remains unset.

Local qualification completed with fresh genuine backend-enabled and backend-disabled full daemon builds: 7 enabled CTests containing 35 cases and 3 disabled-backend CTests containing 12 cases passed. All 320 linked C++ files (319 project files and bundled bech32) were then freshly built with ASan/UBSan; all 35 enabled-backend cases passed. Other external dependencies, Rust, C and PQClean remain uninstrumented, and macOS leak detection was off. The full daemon and disabled-backend binary are outside that sanitizer result.

Three copied header controls independently omitted the legacy-height boundary, node-kind binding, and duplicate-insertion refusal. Each rebuilt the entire 320-file graph and failed its intended named assertion without a fixture exception or sanitizer diagnosis. The unchanged implementation then passed all six catalog cases again. Production code and tests were introduced together; there is no initial pre-implementation failing-test claim. Exact evidence is retained privately. This qualification does not cover the remaining canonical or compact-node integration.
