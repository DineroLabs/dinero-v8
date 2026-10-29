# Replay forest state sharing

## Change

`UtreexoForest` copies and clones retain the same immutable state version until a mutation. A mutating method first allocates and copies the complete state when another forest retains that version, then swaps in the detached copy. Allocation failure leaves existing versions untouched. Roots, nodes, leaf positions, deleted positions, leaf count and canonical-root mode stay in one version.

The guard is inside the forest, so a mutable forest reference acquired before a replay checkpoint still goes through detachment. A lazy guard only at `ConsensusUTXOSet::GetForest()` would not cover such a reference. Existing external locks and replay rollback ownership remain unchanged. This does not make concurrent reads/writes of the same forest object safe.

Copying and moving retain a shared version without allocating. Moved-from objects remain valid and can be reassigned. Height-based canonical-root promotion uses the guarded setter and root rebuild. Deserialization begins with independent new storage. Forest algorithms and serialized formats are unchanged; the implementation transformation was checked against the preceding source after removing storage indirection and detachment scaffolding.

## Qualification so far

Eight fresh ON/OFF syntax checks passed. All 320 project C++ translation units linked into the replay fixture were freshly built with ASan/UBSan, and all 142 selected cases passed, including four new preservation cases and all 138 preceding cases. Three copied implementation omissions failed the intended assertions; the unchanged four cases passed afterward. Five additional fresh ten-project-C++ component graphs passed 77 existing move, proof-generation, mutation, spend-path and checkpoint cases. Their original fixtures and deadlines were retained.

These are scoped local results. External libraries, Rust, C and PQClean were not instrumented in those graphs; macOS leak detection was off. Full normal ON/OFF builds for this revision and optimized cost measurements remain pending when this document is first installed. The daemon, whole-history capacity, other platforms and release readiness are not established by these component results.

## Remaining cost and release work

This removes repeated forest copies before the first mutation; it still copies the whole shared forest on that mutation. It does not establish an asymptotic improvement or measured speedup. The earlier rollback experiment improved failed-block restoration but did not demonstrate a faster common successful path. Fresh measurements must preserve exact coin/forest/tip state and distinguish process memory high-water marks from live memory use.

Historical replay limits, selected-lock proof work, full-history/cadence/load/mixed-peer/crash qualification, platform coverage and final release artifacts remain open. Mainnet activation stays unset. Public publication and CI remain held under the existing test exclusions.
