# Consensus-Orchard separation (sep4)

**Status:** stage-0 inventory complete (2026-11-11, measured at the sep3 stage-D tree with `nm` over the
ON build archives and `ninja -t deps` over the OFF build); design draft.
**Owner approval:** "go ahead with sep4 after sep3 when ready" (2026-11-10).
**Sibling lanes:** sep3 (ambient-globals purity; its P1/K5 discipline applies to every consensus edit
here). Codex's chainstate/storage lane owns the block graph and storage ownership — §6 lists which
Orchard pieces this program deliberately does NOT touch because they are fused with that lane.

## 1. Problem (measured)

Orchard consensus validity is a separate component in everything but declaration:

- 16 `src/consensus/orchard_*.cpp` sources are compiled ONLY by `rust/orchard_backend/CMakeLists.txt`
  into 29 backend STATIC targets; none belongs to `dinero_consensus`/`dinero_consensus_core`/`dinero_core`.
- The backend CMake reaches into targets it does not own: 11 `set_property(SOURCE … DIRECTORY ${ROOT})`
  define injections (`DINERO_HAS_ORCHARD_*` onto relay/scheduler/wallet-service/p2p/daemon sources),
  links pushed into `dinero_core` (8), `dinero_mempool`, `dinerod`, and test targets, plus a
  `file(STRINGS)` read of `include/consensus/tx_validation.h` as a configure dependency.
- Declared 5-target cycle: {`dinero_core`, orchard `chainstate_write`, `reindex`, `runtime_reader`,
  `snapshot_store`} (mutual PUBLIC/PRIVATE edges, backend CMake :262-:466).
- **Undeclared latent cycle:** `dinero_orchard_header` needs 3 symbols only `dinero_core` defines
  (`HeaderChainSelector::GetHeaderValue/GetAsertContextByHash/GetAncestorHashByHash`), contradicting
  the backend CMake's own zero-core-symbols comment (:328-331); it links today only because consumers
  (dinerod, a test that recompiles `header_chain.cpp`) supply the code.
- **The kernel's `dinero_shielded` PUBLIC link resolves ZERO symbols** (`nm`: 0 in either kernel
  archive). The kernel uses only header-only shielded code, yet every kernel user inherits sqlite3,
  `dinero_zk`, `dinero_bip32` and 6 wallet sources at link time.
- Header leakage: 24 of 26 `include/consensus/orchard_*.h` compile into OFF-build objects
  (`orchard_profile.h` alone in 97), 3 Orchard fields live in `ChainParams`, and the backend include
  path is added to every build (root :1272-1273).
- Duplicate objects in the ON dinerod link: `state_commitment`, `snapshot_binding`, `utxo_set_digest`
  are compiled into both core and orchard archives.

## 2. Goal

A declared `dinero_orchard_consensus` library owning the **storage-free Orchard validity set**, with an
explicit, measured interface to the transparent kernel — and the kernel freed of its no-op shielded
link. End state of THIS program:

- New target `dinero_orchard_consensus` (ON builds) holding the symbol-level free-standing clusters:
  `orchard_transparent` + `orchard_authorization` + `orchard_state_transition`;
  `orchard_block_coins` + `orchard_resources` + `orchard_block_filter` + `orchard_forest_transition`;
  `orchard_state_root`; `orchard_block_context`; `orchard_block_reader`;
  `transaction_reader` + `orchard_coin_snapshot` (today: 0 core symbols, 0 chainstate symbols — §8 of
  the inventory). The backend targets that only re-group these become links to the new library.
- Header hygiene for that set: `orchard_state_transition.h` / `orchard_state_root.h` stop including
  storage headers, and `orchard_catalog_coin_view.h`'s reach into `storage/orchard_catalog_state.h`
  is broken for the block_coins cluster (interface types move or get forward-declared adapters).
- `dinero_consensus_core` drops the `dinero_shielded` PUBLIC link (zero resolved symbols); the two
  header-only uses (`ShieldedEpochSnapshot`, `compact.h` inlines) keep compiling via includes alone.
  Real shielded users (`dinero_consensus` members `block_validation`/`reindexer` live in core's link;
  orchard staging/reindex) keep their own edges.
- A gate (**O1**) makes the boundary regress-proof: the new library's archives must need **zero**
  symbols defined only by `dinero_core`/`dinero_chainstate`/`dinero_shielded` (nm-based, modeled on
  K3/check_symbols), plus a header-reach baseline for the library's sources (CV1 bucket semantics).

## 3. Non-goals (documented follow-ups, not silent omissions)

- **Everything storage-fused** stays with the backend targets and the Codex chainstate lane:
  `orchard_block_staging` (50 chainstate symbols, `*UnderLock` API on ChainDB/BlockStorage/
  ChainWriteToken), `chainstate_write`, `reindex`, `snapshot_store`, `runtime_reader`,
  `orchard_snapshot*`, `legacy_accounting`, and the declared 5-target cycle — breaking that cycle IS
  chainstate-ownership work.
- **`orchard_header`'s back-edge**: its 3 missing symbols are `HeaderChainSelector` = the block graph
  = Codex's lane. Recorded here; fixed there (or in a coordinated slice).
- **ChainParams' 3 Orchard fields** stay (they are chain parameters; sep3 threads them properly).
- **OFF-build header leakage** beyond the new library's set (e.g. `orchard_profile.h` in 97 objects)
  is tracked, not eliminated here.
- The 11 define injections are reduced only where they target files this program moves; the
  relay/p2p/wallet-service injections stay until their owners' lanes touch those files.
- No behavior change anywhere; sep3's verification bar (gate-faithful builds, K1-K5, 24/24 consensus
  vectors, targeted ctests, neuter-proofs) applies to every stage.

## 4. The O1 gate

`scripts/ci/separation/check_orchard_boundary.py` + pytest + an always-run single job in
`separation-purity-gate.yml`'s workflow family (or its own F1-pattern workflow):

- **Symbol tier:** for each archive of `dinero_orchard_consensus`: undefined symbols must resolve
  within {the library itself, dinero_consensus/_core, shielded-free primitives/crypto/common, the
  Rust FFI import} — never from `dinero_core`, `dinero_chainstate`, or `dinero_shielded`.
  (nm -A -u against a committed deny-list of definer archives, K3-style.)
- **Header tier:** per-source include-reach baseline (preprocessor `-MM -MG`, CV1 mechanics) with
  storage/daemon/wallet families forbidden; committed baseline may only shrink (P1 ratchet semantics).
- Self-tests: positive control, neuter test, stale-baseline live test — same suite shape as P1's.

## 5. Stages

- **A — doc + free win:** this document; drop the kernel's zero-symbol `dinero_shielded` PUBLIC link
  (keep includes); prove with `nm` + full local verification that nothing changes. K5 re-anchor if any
  frozen source is touched (expected: CMake only — not frozen).
- **B — gate first:** O1 checker + tests + workflow, baselined against the CURRENT backend targets for
  the free-standing set (report-only rows for the known dirt, e.g. the header back-edge if measured).
- **C — the library:** create `dinero_orchard_consensus` with the §2 source set; re-point the backend
  re-group targets and consumers; fix the named header-level storage includes; duplicate-object cleanup
  for `transaction_reader`/`coin_snapshot` if the move makes it free. O1 baseline ratchets to clean.
- **D — switch hygiene (owned files only):** replace define injections that target sources this
  program now owns with the library's own interface header; regenerate baselines.
- **E — enforcement + report:** O1 required after observed green on main; final program report;
  explicit unblock notice for any coordinated follow-ups.

## 6. Collision ledger

- **Codex chainstate/storage lane:** the block-graph split touches `block_index`/`header_chain` and
  later the staging/reindex/snapshot cluster. sep4 stages A-D touch NONE of: block_download_scheduler,
  block_staging, chainstate_write, reindex, snapshot_store, runtime_reader, orchard_header,
  storage/orchard_state.cpp, legacy_retirement.cpp. The `orchard_header` back-edge fix is explicitly
  ceded/coordinated.
- **sep3:** lands first; every sep4 consensus-source edit follows the K5 re-anchor discipline and the
  P1 zero-baseline (any file moved into gated sets must be ambient-free).
- **#895 (SDK) / phone program:** no path overlap; sep4 never touches deployment or publishers.
- Stale docs to fix opportunistically in stage A: wallet-sep design §cycle anchors (:349/:451 cite
  pre-2026-11-08 lines and a removed `_header` PRIVATE edge); the missing
  `task-6.3-owner-decision-orchard-header.md` cited at backend CMake :367.
