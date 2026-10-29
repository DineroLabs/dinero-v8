# Exact block rollback for independent replay

## Purpose and ownership

Independent genesis-to-parent replay previously asked `BlockValidator` to deep-copy the complete UTXO map and serialize the complete forest before each block. The replay engine discarded the optional full snapshot when retaining its bounded undo tail, after already paying that capture cost.

`ConsensusUTXOSet::CreateForReplay()` selects a scoped `BlockConnectRollback` owner. Ordinary construction and other backends retain their existing Snapshot/Restore path. The backend must outlive the token; actual replay uses the token synchronously on its private state. This does not establish a new concurrent mutation API.

The token copies each coin's original value only before its first mutation, or records that the key was absent. It saves exact height, hash and forest state. The forest copy remains complete, including roots, nodes, leaf positions, deleted positions and historical canonical-root mode, under the existing shared forest mutex. Existing guarded forest writers retain their mutex. No checkpoint lock spans body/script/proof validation.

On failure, the token erases originally absent keys first, transfers preallocated original coin-map nodes back, moves the saved forest back under the existing writer mutex and restores metadata. The map does not shrink its bucket array during the token lifetime; after removing newly created keys, restoration fits within its original capacity. An internal coin-count mismatch terminates instead of publishing an unexplained partial rollback. Allocation guarantees and the backend/token lifetime remain part of the owner contract, not permission to bypass validation.

The validator acquires the token before changing canonical-root mode. It retains structural/body/script/proof and shielded validation, existing failure handling and both success paths. Successful connection commits the token only after the existing work succeeds. Nested checkpoints and bulk Snapshot/Restore/Clear/Load/Apply/Undo operations refuse while the owner is active. Mutations through existing guarded forest operations remain reversible using the saved full forest.

## Durable undo and recovery

This token is an in-memory rollback owner, not a serialized undo substitute. The replay engine still retains ordinary spent-coin/frontier/anchor undo and Utreexo delta material in its existing bounded tail; no account, wallet or canonical receipt is invented. Full map snapshots remain available to ordinary backends. Replay header validation and authenticated spool publication remain unchanged.

## Verification scope

The new fixtures exercise first-touch coin recording amid 20,000 unrelated coins, exact forest deletion/mode/metadata restoration, commit and nested/bulk refusal, actual validator late failures followed by retry, and every prefix of a 20-block replay against an ordinary reference, including exact serialized undo bytes. The 20,000-coin mechanics case has a small explicit forest and is not a representative forest-cost benchmark.

The first qualification attempt compiled all320 linked project C++ files but failed the new exact undo comparison: the reference fixture omitted independent shielded state present in the real replay engine. The repaired fixture wires its own tree, in-memory nullifier set and anchor history, explicitly selects stateful validation, and preserves every original assertion. It adds initial owner and per-prefix frontier/anchor comparisons. All 138 selected cases passed on a fresh sanitizer graph covering all 320 linked project C++ files, including these four new cases. Three copied, serialized implementation omissions each failed at the intended assertion; the unchanged four cases then passed. Eight ON/OFF syntax checks also passed. External/Rust/C/PQClean dependencies were uninstrumented and macOS leak detection was disabled. Fresh full normal ON/OFF builds and their selected runtime checks remain required.

## Remaining limits

Full forest cloning and other validation forest copies still occur. Net time and memory improvement have not been measured. The private bounded checkpoint measurement must compare coherent coins and forests using identical optimized source; sanitizer timings are not production performance. Process high-water RSS is not live checkpoint memory. Full replay throughput, peak memory, disk usage, large historical branches and failure/load behavior need separate qualification.

This does not raise the existing 100000-block or 256MiB material limits, justify activation at 125000, repair unrelated selected-lock proof paths, or establish whole-node release readiness. Public publication/CI remains held and mainnet activation remains unset.
