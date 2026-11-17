# Chainstate and storage ownership

Status: staged separation plan. Stage 1 moves restart metadata into the existing
`dinero_chainstate` target. Stage 2 records the shared block graph's current
owners and raw consumers. Later stages need their own review and qualification.

## Present boundary

`dinero_chainstate` already owns ChainDB, coins storage/cache, the UTXO position
index, Orchard state, block files, and forest checkpoint restore/retention.
`dinero_core` still owns `ChainstateService`, activation/reorg transitions,
AssumeUTXO replay, header sync, and the shared block graph. The block graph's
index, candidate, and orphan globals live in `src/consensus/block_index.cpp`.
That file is intentionally compiled into both `dinero_core` and
`dinero_consensus` for different consumers; it cannot simply be added to a
third library. `block_lifecycle.cpp` shares its mutex and invalidity state.

The public boundary should eventually offer coherent chain snapshots for
readers and one owner for transitions. A library move alone does not establish
that boundary. In particular, storage calls from a reorg must keep their
existing atomic batch and rollback semantics.

## Stages

1. Move `chainstate_metadata.cpp` from core's storage-glue list into
   `dinero_chainstate`. Its live consumer is `HeaderSyncManager`; the disk
   format and call sites stay unchanged. Qualify restart metadata, daemon
   linking, and header-sync restart with Orchard on and off.
2. Record every CMake compile of `block_index.cpp` and `block_lifecycle.cpp`,
   including standalone consensus and tests, plus direct raw-global uses.
   Ratchet those counts so a new owner or consumer fails locally. This is an
   inventory guard, not proof that chainstate already owns the graph.
3. Separate pure index-record operations from mutable graph operations before
   changing the library owner. Preserve the existing lock order and the
   equal-work candidate ordering. Prove the standalone consensus link and
   daemon link after the split.
4. Put graph mutation and active-tip publication behind a chainstate-owned
   interface. Move consumers incrementally, with restart, invalid-candidate,
   equal-work, and deep-reorg regressions at each step. Do not replace one
   shared global with another singleton.
5. Move the activation/reorg coordinator only after its wallet, mempool, pool,
   network, and RPC callbacks are explicit dependencies. Keep block connect,
   coins, checkpoints, undo, and selected-tip publication atomic. Qualify
   full-node and CSN parity, crash/restart, snapshot recovery, and real daemon
   operation before retiring the old owner.

The consensus-globals purity program's Stage E moves validation/header files
out of core and edits the root CMake source list. This program will rebase its
small CMake edits on that stage, and will not move those sources or edit
`chainstate_service.cpp` concurrently with Stage E. No stage changes consensus
rules or persisted record formats merely to change library ownership.
