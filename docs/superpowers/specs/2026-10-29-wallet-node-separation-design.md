# Consensus kernel and wallet separation — design (rev 2)

**Status:** draft for owner approval. No implementation until approved.
**Release:** separate from v8.2.0 and from PR #855. Whether any of it fits v8.2.1 is decided **after stage 0 measures the scope** (section 11).
**Baseline:** `dinero-main` 3aca093c0 (after #856). File:line references are against that commit.
**Rev 2 changes:** owner chose to purify `dinero_consensus` itself (not to route the wallet through `dinero_consensus_core`); added the target architecture; folded in the builder check (rescan keeps a per-call chain view; mempool adapter is header-only; consensus_core is not self-contained and is out of scope).

## 1. Target architecture (owner)

Clear ownership and one-way dependencies. **"Pure" means code that can run from explicit inputs without a database, network, or global node state.**

- **Consensus kernel:** deterministic validation, scripts, sighash, subsidies, chain rules. Receives height, parameters and coin data as inputs. Pure and independently linkable.
- **Chainstate:** stores blocks and UTXOs; adapts them to consensus interfaces.
- **Mempool:** owns unconfirmed transactions and policy; calls consensus rules; consensus never calls the mempool.
- **P2P:** transports messages; owns no validation rules.
- **Wallet:** owns keys, accounts, wallet data. Node-side adapters provide chain and mempool observations.
- **Daemon:** assembles the components. `dinero_core` may orchestrate services and need not be pure.

Forced-link tests and dependency-graph checks keep each boundary honest. The destination is reached in reviewable stages. **This project is the first leg: a pure consensus kernel and a wallet that depends only on pure libraries.** Mempool-as-a-library, P2P, and daemon assembly are later legs (section 13).

## 2. Why now

`dinero_wallet` declares pure dependencies, but ten wallet objects call node code, and `dinero_consensus` — which the wallet links — carries RocksDB, SQLite, ChainDB, ChainstateService, and wallet code. Those references resolve only by link-order luck. On GCC 15 the luck runs out (8 test links fail in CI; 114 in a forced worst case). PR #856 pinned three jobs to ubuntu-24.04 (`.github/workflows/tests.yml:42`, `:780`, `core-heavy.yml:49`). **That pin is a workaround; a 24.04 pass says nothing about GCC 15. The defect is open until those jobs pass on GCC 15 unpinned.**

## 3. Goals

1. `dinero_consensus` is pure by the definition above and links on its own (forced, whole-archive) with nothing but declared pure libraries.
2. The consensus↔wallet address cycle is gone.
3. `dinero_wallet` links on its own (forced, whole-archive), and its **full transitive closure** contains no RocksDB, chainstate, core, daemon, mempool, P2P, or RPC code.
4. Both properties are proven mechanically and enforced in CI (section 8).
5. Callers migrate with no external behavior change (section 4, section 10).
6. The 24.04 pin comes off only after the three jobs pass on GCC 15 without it.

## 4. Hard constraints (owner)

- **Wallet files open unchanged, no migration.** Wallet storage is SQLite only (`<datadir>/wallet_registry.db`, `wallet_manager.cpp:396`; `<datadir>/wallets/wallet_<name>.db`, `:1695`, `:1923`; schema by `PRAGMA user_version`, `:2827`, `:2840`). No schema, path, or PRAGMA change. `open_sqlite`'s PRAGMAs (`src/sqlite_open.cpp:19-40`) move byte-for-byte; raw `sqlite3_open` sites stay as they are.
- **RPC methods, fields, and error behavior unchanged. FFI signatures unchanged** (`include/nodecore/nodecore_ffi.h`, `wallet-core/ffi/wallet_ffi.h`, 7 JNI exports). **Qt and NodeCore behave the same.**
- **No consensus logic change.** No edit to any file in `CONSENSUS_MANIFEST` (`include/consensus/freeze.h:133-141`: `limits.h`, `tx_validation.h`, `subsidy.h`, `freeze.h`). Consensus sources that stay in the library are not edited, with one named exception (`tx_validation.cpp` include line, section 6.2).
- Internal C++ interfaces may change.
- **Any unavoidable external change is flagged for separate decision**, never folded in (section 10.3).

## 5. Non-goals and anti-goals

- No new phone app, light client, or FFI surface. No behavior change in validation, coin selection, rescan, fee bumping, RBF/CPFP, or sync reporting.
- Dead code is moved or left, not deleted, in this project; deletion is a follow-up (keeps diffs reviewable).
- **Anti-goal:** a gate that passes because of stubs or lucky link order.
- **Anti-goal:** moving a dependency instead of removing it — e.g. a library the wallet links that itself drags RocksDB back in.

## 6. Leg 1a — purify `dinero_consensus`

### 6.1 Measured classification

A read-only survey classified all 52 sources (`src/consensus/CMakeLists.txt:137-190`; no conditional sources) by full transitive includes (`c++ -M`) and by unresolved symbols in a built archive, cross-checked against `undeclared.json` (16 node symbols; exact match).

- **Pure (42):** all script, sighash, tapscript, covenant, PoW, chainwork, merkle, filter, Utreexo, CSN-replay, subsidy, chainparams, primitives, and hash sources. `src/common/serialization.cpp` includes `storage/tip_info.h`, which is header-only over chainwork/uint256 — pure in practice; the header's location is a follow-up.
- **Impure (10 objects, 8 distinct problems):**

| Source | Problem | Live consumers | Disposition |
|---|---|---|---|
| `coins_db.cpp` | RocksDB (`:2-3`, `DB::Open :31`, WriteBatch `:354`, Iterator `:573`) | none compiled (only uncompiled `src/mining/miner_engine.cpp:356,462,512`) | **move to `dinero_chainstate`** (already links RocksDB; no chainstate code calls it → no cycle) |
| `coins_view_cache.cpp` | includes `coins_db.h` (RocksDB headers) | none compiled | **move to `dinero_chainstate`** with coins_db |
| `tx_validation.cpp` | `#include "consensus/coins_db.h"` (`:2`) only for the `CoinsViewCache` type (virtual `getCoin`, `:97`, `:221`); no RocksDB symbols | `test_covenant_activation.cpp:460,471` (`consensus::verifyScript`) | **stays.** New RocksDB-free header `include/consensus/coins_view.h` declares `CoinsViewCache`; `coins_db.h` includes it; `tx_validation.cpp` includes it instead. Frozen `tx_validation.h` already forward-declares the type (`:33`) and is not touched |
| `global_utxo_set_impl.cpp` | `daemon/services/chainstate_service.h` (`:11`) → p2p, ChainDB, UTXOIndex, sqlite3; calls `ChainstateService::GetChainDB`/`getBlockHeight` (`:38,40`), `ChainDB::getCoinWithConfidentialFallback` (`:58,75`) | **none compiled** — callers `fast_sync_service.cpp:321`, `methods_snapshot.cpp:132,312` are in no CMake file (verified) | **move to `dinero_core`** (the only place it can live; it needs ChainstateService) |
| `src/indexing/utxo_position_index.cpp` | `storage/chain_db.h` (RocksDB) `:12`; `ChainDB::forEachUTXO` `:97,203`, `getPreBaseCoinSetBase :175`, `forEachPreBaseCoin :205` | `chainstate_service.cpp` (core) `:2801, 4820-4863, 12065, 14175, 14200, 15863, 15880`; `methods_utreexo.cpp:517,887`; tests `test_utxo_position_index_rebuild` (`tests/CMakeLists.txt:5221-5249`), `test_csn_integration` (`:10616-10638`) — both already link chainstate | **move to `dinero_chainstate`** (core links chainstate, `CMakeLists.txt:1194`; chainstate never calls it → no cycle) |
| `addr_codec.cpp` | `wallet/address.h` (`:2`) → UTXOIndex, wallet balance service, sqlite3; calls 5 `Address::*` from the wallet | **no consensus source calls it**; already compiled into `dinero_wallet` (`src/wallet/CMakeLists.txt:13`), `dinero_core` (`:958`), and 7 test executables | **remove from consensus** (section 6.3) |
| `block_index.cpp`, `sigops.cpp` | logger only | — | **stay**; logger becomes a declared library (section 6.4) |

Interface vs. input (owner's kernel rule): none of the remaining 44 sources reads a database or node global after this; the only coin access left in the kernel is through the `CoinsViewCache` abstraction passed into `tx_validation`, i.e. chainstate adapts to a consensus interface. No new consensus interface is needed in leg 1.

### 6.2 Library interface cleanup

After the moves, `dinero_consensus` drops from its PUBLIC usage requirements:
- `RocksDB::rocksdb` (`:217` Apple, `:230` other) and the RocksDB include dir (`:193-195`);
- `sqlite3` (`:218`, `:231`) — needed only by the addr_codec and global_utxo_set_impl header chains;
- the `src/daemon` include dir (`:199`) — no consensus source needs it (all 52 preprocess without it).

`jsoncpp_static` stays (`block_undo.cpp:3`).

**Consumers that may have relied on those transitive exports** (they link `dinero_consensus` and get RocksDB/sqlite/daemon includes or symbols through it): `dinero_chainstate` (`src/storage/CMakeLists.txt:33`, declares RocksDB itself at `:37`), `dinero_core` (`CMakeLists.txt:1195`), `src/pool:27`, `src/network:26`, `tools/CMakeLists.txt:8,14,28`, `cmake/SwapTower.cmake:21`, `wallet-core/ffi/tests:14`, 4 fuzz targets, ~236 test references (`tests/CMakeLists.txt` 217, `tests/consensus` 19), and the Rust bridge (`mobile-tauri/src-tauri/build.rs:29-38`). Stage 0 measures exactly which of these break (section 11); each fix is the consumer declaring what it uses.

### 6.3 Address cycle

`addr_codec.o` is the only consensus object that needs wallet symbols (`Address::validateAddress`, `getAddressMetadata`, `decodeAddress`, `createAddress`, `createBech32P2WPKH`; defined `src/wallet/address.cpp:242, 343, 556, 791, 1499`). Since nothing in consensus calls addr_codec, removing it from the consensus source list breaks the cycle without moving `address.cpp`. Its other two homes (wallet, core) are unaffected. `addr_types.cpp` is pure and stays.

Residual edge, not a cycle: `dinero_address_encoding` (root `:269-273`) uses `HrpForActiveNetworkRef` from consensus without declaring it. Stage 1 adds the declaration (`dinero_address_encoding` → `dinero_consensus`), which is one-way.

### 6.4 Logger

`block_index.cpp` and `sigops.cpp` (sigops is also in `dinero_consensus_core`, `:64`) use `Logger`/`g_logger`, which consensus never declares; today the wallet's or core's copy of `src/common/logger.cpp` satisfies them. A pure kernel must declare everything it needs, so rev 1's "don't move the logger" is reversed: **new `dinero_logging` library** containing only `src/common/logger.cpp` (std + `<filesystem>` only). Linked PUBLIC by `dinero_consensus`, `dinero_consensus_core`, `dinero_wallet`, `dinero_core`; removed from the wallet's and core's source lists so it exists once in each archive closure. The 132 test executables that compile `logger.cpp` directly are left alone (their own definition wins; the archive member is not pulled).

Whether a process-wide logger counts as "global node state" under the purity rule is noted as a follow-up question; in leg 1 it is a declared, storage-free dependency.

## 7. Leg 1b — separate the wallet

### 7.1 What the wallet actually calls

Derived from the code (two surveys, reconciled with `undeclared.json` and `link-red-base.log`). Ten wallet objects reach node code.

| # | Operation | Call sites | Today's provider |
|---|---|---|---|
| C1 | Chain tip (height, hash) | wallet_manager:5682, wallet_sync_status:25, reorg_detector:104; hd_wallet:1070,1214 via existing `ChainHeightProvider` | ChainDB `getTip` |
| C2 | Block hash at height | wallet_manager:5831 | ChainDB `getBlockHashByHeight` |
| C3 | Read full block | wallet_manager:5839 → `storage::ReadArchivalBlock` (`include/storage/archival_block_reader.h:70-105`) | ChainDB + optional BlockStorage |
| W1–W3 | Watch-index lookup, list unspent, register/clear watch scripts | wallet_manager:4395, 2547, 5262, 5503, 8189, 8297, 8489, 8634, 8679; hd_wallet:1018-1050, 1073, 1218, 1490; transaction_builder:95 | UTXOIndex (SQLite, `src/wallet/utxo_index.cpp`, compiled into chainstate) |
| S1 | Open SQLite with standard PRAGMAs | wallet_manager:401, 1712, 1941; descriptor_store:20 | `open_sqlite` (in chainstate) |
| M1 | Outpoint spent in mempool | wallet_manager:4454 | `Mempool::isOutputSpentInMempool` |
| M2 | Mempool entry (fee, size, vin, depends) | rbf_cpfp_detector:85,141,182,261; fee_bump_engine:190,260 | `Mempool::getMempoolEntry` |
| M3 | Mempool stats | slow_reason_analyzer:210 | `Mempool::getStats` |
| D1–D3 | Sync phase/header progress; peer quality; tx inclusion | wallet_sync_status:44,54; slow_reason_analyzer:141; wallet_fee_bump_hooks:26-27,106,172,214,244,311 | core / p2p / mining |
| P1 | Address → scriptPubKey | descriptor_psbt:253,391; recipient_descriptor:18 | `BuildScriptPubKeyFromAddress` (`src/common/address_script_builder.cpp`, core, pure) |

Compile-time leaks to remove: `storage/chain_db.h:8-13` (RocksDB headers) included by four wallet sources; `daemon/mempool.h:20-21`; node types in `include/wallet/wallet_manager.h:42-46,139,399,580-581`, `wallet_sync_status.h:3`, `wallet_fee_bump_hooks.h:7`. Not in the set: tx submit (the wallet makes none; `IMempoolInterface`, `include/wallet/mempool_interface.h:137`, is left alone).

### 7.2 Shape

```
dinero_logging      (new)  src/common/logger.cpp
dinero_sqlite       (new)  src/sqlite_open.cpp, unchanged; linked by wallet and chainstate
dinero_consensus           pure after leg 1a
dinero_wallet              pure: links consensus, logging, sqlite, crypto, bip32, tx_primitives,
                           address_encoding, shielded, argon2, OpenSSL, sqlite3
                           + utxo_index.cpp (from chainstate), address_script_builder.cpp (from core)
                           defines IWalletChainView, IWalletMempoolView
dinero_wallet_node  (new)  ChainDbWalletView adapter; wallet_sync_status, slow_reason_analyzer,
                           wallet_fee_bump_hooks (moved out of the wallet); links wallet, chainstate, core
include/daemon/mempool_wallet_view.h   header-only MempoolWalletView adapter
```

- **UTXOIndex** is SQLite (the RocksDB comment at `src/storage/CMakeLists.txt:17` is wrong) and includes only wallet/address/logger headers; it moves into the wallet, so W1–W3 become internal calls. The node still creates and owns the instance (`chainstate_service.cpp:2595`, `daemon_app.cpp:1724`) and hands it in with `setUTXOIndex`. If `dinero_chainstate`'s own objects use UTXOIndex, chainstate links the wallet for it (builder checks).
- **Interfaces** (`include/wallet/node_interfaces.h`), only what 7.1 requires:

```cpp
struct WalletChainTip { int32_t height; uint256 hash; };
class IWalletChainView {                  // C1-C3
public:
    virtual ~IWalletChainView() = default;
    virtual std::optional<WalletChainTip> GetTip() const = 0;
    virtual std::optional<uint256> GetBlockHashAtHeight(int32_t height) const = 0;
    virtual std::optional<CBlock> ReadBlock(const uint256& hash) const = 0;
};
class IWalletMempoolView {                // M1-M3
public:
    virtual ~IWalletMempoolView() = default;
    virtual bool IsOutputSpent(const OutPoint&) const = 0;
    virtual std::optional<WalletMempoolEntry> GetEntry(const uint256& txid) const = 0;  // fee, tx_size, tx.vin, depends
    virtual WalletMempoolStats GetStats() const = 0;                                    // total_size, tx_count, avg_fee_rate
};
```
Field types are copied from the current Mempool/ChainDB types. A null view reproduces today's null-pointer behavior.
- **Rescan keeps a per-call chain view** (builder finding): `rescanBlockchain(start, gap, const IWalletChainView&)`. `wallet_worker.cpp:186` (compiled into core, `CMakeLists.txt:1136`) passes no BlockStorage, so `ReadArchivalBlock` reads ChainDB only; it gets a view built without BlockStorage and keeps reading the same source. The other callers (`wallet_service.cpp:502`, `methods_wallet_context.cpp:6678`, `nodecore_ffi.cpp:1134`) get views with BlockStorage, as today. A stored view would have silently changed `wallet_worker`'s read path.
- **Mempool adapter is header-only** (builder finding): `src/daemon/mempool.cpp` is in no library, and `dinero_rpc_handlers` already calls `Mempool` directly without declaring it. A header-only adapter adds no new undeclared edge; the rpc_handlers edge is pre-existing and listed in leg 2. It is outside the wallet's closure, so it does not weaken the wallet gates.
- **Node diagnostics move out** (D1–D3): `wallet_sync_status`, `slow_reason_analyzer`, `wallet_fee_bump_hooks` go to `dinero_wallet_node` unchanged, rather than putting node concepts into wallet interfaces. `reorg_detector` (C1 only), `rbf_cpfp_detector`, `fee_bump_engine` stay in the wallet on the interfaces. *My call.*

## 8. Gates (written first, observed RED on the baseline)

Each gate is added in stage 0 as a non-required job so its RED is recorded, and becomes required at stage 7.

| Gate | What | Baseline expectation |
|---|---|---|
| **K1 consensus forced link** | executable linking `--whole-archive libdinero_consensus.a` + only its declared deps (`--no-undefined` semantics); `main` calls `Params()` and verifies one known script and sighash vector | RED: 16 undefined node symbols (ChainstateService, ChainDB, Address, Logger) |
| **K2 consensus closure** | CMake script walks `dinero_consensus` usage requirements recursively; fails on RocksDB, sqlite3, chainstate, core, wallet, rpc, any target with `src/daemon/mempool.cpp`, or RocksDB/`src/daemon` include dirs | RED: RocksDB, sqlite3, daemon include |
| **K3 consensus symbols** | `nm -C` on K1's output: no `rocksdb::`, `sqlite3_`, `ChainDB`, `ChainstateService`, `Address::`, `Mempool` | RED |
| **K4 consensus vectors identical** | before/after run of: `BIP341SighashVectors` (`tests/CMakeLists.txt:5601`), `BIP119CTVVectors` (`:5558`), `CovenantActivation` (`:5622`), `ScriptJSONTests` (`:6081`), `DAAGoldenVectors` (`:1929`), `UtreexoLeafHashGolden` (`:10846`), `MerkleGolden` (`:9335`), `test_header_hash_vectors` (`:9198`), `HeaderPoWVerification` (`:9395`), `TaprootConsensus` (`:5470`), `ScriptInterpreter` (`:5490`), `TaprootScriptPathConsensus` (`:5579`), `RIPEMD160*Vectors` (`:261,760,775`), `HashWrapperVectors` (`:792`), `StateCommitmentExternalVectors` (`:2288`), `CompactRegtestFixedVectors`/`VectorOracle` (`:13499,13510`), `SerializationVectors` (`tests/consensus/CMakeLists.txt:699`), `ConsensusCoreSt` (`:1344`), `StatelessVerification` (`:1229`), `SubsidySchedule` (`:1222`), `ConsensusInvariants` (`:1267`), `FormalInvariants` (`:1155`), `Consensus_DeterminismOracles` (`:639`), plus `compact-regtest-vectors.yml` and the independent-vectors / saved-proofs jobs. Pass on both, and every test that prints a vector digest prints the same digest | GREEN on baseline (reference run) |
| **K5 consensus source freeze** | `CONSENSUS_MANIFEST` checksums verify; `git diff` of `src/consensus/**` limited to CMake lists, the new `coins_view.h`, `coins_db.h` including it, and the one include line in `tx_validation.cpp` | GREEN |
| **W1 wallet forced link** | `--whole-archive libdinero_wallet.a` + only declared deps; no `wallet_test_deps`, no `mempool_test_stubs.cpp`; `main` constructs a `WalletManager` on a temp dir and opens it. Evolves the evidence-folder probe | RED: at least the 10 undefined refs seen for wallet_manager.o (Mempool, ChainDB, BlockStorage, UTXOIndex, open_sqlite), plus the core/p2p/mining symbols of D1–D3 |
| **W2 wallet closure** | as K2 for `dinero_wallet`; additionally fails on full `dinero_core`, `dinero_rpc_handlers`, P2P. Allows third-party crypto (`lightning_core_static` = libwally/secp256k1-zkp/blake3, via `dinero_zk`) | RED: RocksDB via consensus |
| **W3 wallet symbols** | as K3 on W1's output, plus `BlockStorage`, `HeaderSyncManager`, `PeerManager`, `TxInclusionAnalyzer` | RED |
| **W4 wallet-file compatibility** | fixture datadir (registry + one wallet db + HD wallet + descriptor store) created by the baseline build, checked in; post-split build opens it read-write and reports identical addresses, balance, UTXOs, `user_version`; `sqlite_master` dump identical before and after | GREEN on baseline |
| **W5 external interface freeze** | empty diff of `nodecore_ffi.h`, `wallet_ffi.h`, JNI export list; exported dynamic symbols of `nodecore_ffi` and `dinero_wallet_ffi` unchanged; recorded regtest transcript (getbalance, getnewaddress, listunspent incl. mempool-spent filtering, sendtoaddress, rescan, fee bump/RBF, sync status, 3 error cases) identical field-for-field | GREEN on baseline |
| **W6 standalone consumer** | `dinero_wallet_ffi` and the Rust bridge's static set link with no node libraries | **measured in stage 0** (survey infers RED) |

**Cross-platform:** K1–K3 and W1–W3 run on Linux GCC 15 (ubuntu-latest) and GCC 13 (24.04), macOS Clang (arm64), and Windows MSVC. Windows uses a CI-only workflow modeled on `windows-openssl-release-smoke.yml` (no signing, no uploads — `windows-release.yml` signs when `WINDOWS_SIGNING_READY=true` and is not used). MSVC-specific watch item: the `ParamsImpl` LNK2005 history (root `CMakeLists.txt:520-534`; `tx_validation.o` references `detail::ParamsImpl`). Apple/MSVC forced-link flags are `-force_load` / `/WHOLEARCHIVE`.

## 9. Affected consumers (leg 1a and 1b together)

| Consumer | Change | Behavior |
|---|---|---|
| `dinero_chainstate` | gains `coins_db.cpp`, `coins_view_cache.cpp`, `utxo_position_index.cpp`; links `dinero_sqlite`; loses `utxo_index.cpp` (to wallet) | unchanged |
| `dinero_core` | gains `global_utxo_set_impl.cpp`; loses `address_script_builder.cpp`, `logger.cpp` (to libraries it links) | unchanged |
| `dinerod`, `nodecore_ffi`, NodeCore xcframework (`build_nodecore_xcframework.sh:254-374`) | link set gains `dinero_logging`, `dinero_sqlite`, `dinero_wallet_node`; xcframework script bundles them | unchanged; W5 |
| `WalletService` (`src/daemon/services/wallet_service.cpp`) | builds chain views from `chainstate_->GetChainDB()` + `block_storage_`; passes per call | unchanged |
| RPC (`dinero_rpc_handlers`) | `methods_wallet_context.cpp:1890-1908, 6678-6682`, `wallet_sync_rpc_handlers.cpp:36,44`, `fee_bump_rpc_handlers.cpp:43,51,63-75` pass views. **Lines matched by `qt/tests/test_shielded_rpc_status.cmake:7-39` are not edited** | unchanged; W5 |
| NodeCore C ABI (`nodecore_ffi.cpp:1118-1161`) | rescan builds a view; list-unspent still passes no mempool view (identical output) | unchanged; W5 |
| Standalone wallet FFI (`wallet-core/ffi`) | none in code | may start linking (E1) |
| Rust/Tauri bridge (`mobile-tauri/src-tauri/build.rs:29-38`) | adds `dinero_logging`, `dinero_sqlite` to its static set | may start linking (E1) |
| Qt (`qt/`) | none — separate process over JSON-RPC/WS | unchanged |
| Tools, pool, network, SwapTower, fuzz, ffi tests | declare RocksDB/sqlite/daemon includes themselves where they relied on consensus exporting them (measured in stage 0) | unchanged |
| Tests (~88 link the wallet; ~236 link consensus) | pure-wallet tests drop `wallet_test_deps`/`mempool_test_stubs.cpp` and link the wallet alone; tests needing a mempool use a fake `IWalletMempoolView`; node-integration tests link `dinero_wallet_node`; consensus-linking tests declare what they used transitively | unchanged |

### 9.1 Flagged for separate decision (not folded in)
- **E1.** If W6 is RED today, the split makes the standalone wallet FFI / Rust bridge link. That is a fix, but it changes what that build produces.
- **E2.** `getBalance(const void* mempool_ptr)` ignores its argument (`wallet_manager.cpp:3933`). Left as is.

## 10. Stages

Each stage is its own PR onto `dinero-main`, green on existing CI plus the gates listed, before the next starts. Every gate is observed failing before the change that turns it green; logs are grepped to show tests ran; touched sources are rebuilt from clean.

| Stage | Content | Exit |
|---|---|---|
| **0 — gates + measurement** | Add K1–K5, W1–W6 (non-required); W4 fixture and W5 transcript from the baseline; K4 reference digests. **Scope measurement:** on a throwaway branch, apply only 6.2 (drop RocksDB/sqlite/daemon exports from consensus) and record which consumers fail to build, per platform | K1–K3, W1–W3 RED as expected; K4, K5, W4, W5 GREEN; W6 and the consumer-breakage list recorded → **owner decides v8.2.1 fit** |
| 1 | `dinero_logging`; consensus, consensus_core, wallet, core declare it; `dinero_address_encoding` declares consensus | full CI; K4 identical |
| 2 | Consensus moves (6.1): coins_db/coins_view_cache/utxo_position_index → chainstate; global_utxo_set_impl → core; addr_codec out; `coins_view.h` | K1, K3 GREEN; K4, K5 identical on all four platforms |
| 3 | Drop consensus exports (6.2); consumers declare their own deps | K2 GREEN; full CI on all platforms |
| 4 | `dinero_sqlite`; `address_script_builder.cpp` and `utxo_index.cpp` → wallet | W4 GREEN; full CI |
| 5 | Interfaces, adapters, wallet sources and headers rid of node includes; caller migration (section 9) | W1–W3 GREEN; W4, W5 GREEN |
| 6 | Diagnostics → `dinero_wallet_node`; test link cleanup | all gates GREEN on all four platforms |
| 7 | Gates required. Unpinned trial copies of the three pinned jobs on ubuntu-latest (non-required) | trial jobs GREEN on GCC 15 |
| 8 | Only after stage 7's trials pass: remove the pin and comment at `tests.yml:42`, `:780`, `core-heavy.yml:49`; drop trial copies | the three jobs GREEN on ubuntu-latest |

## 11. Scope measurement and the v8.2.1 decision

Known now: 10 impure consensus objects, of which 4 are moves with live consumers or callers (utxo_position_index, tx_validation include, addr_codec removal, logger) and 3 have no compiled caller (coins_db, coins_view_cache, global_utxo_set_impl); 10 wallet objects; ~5 production consumer areas; ~324 test references. **Unknown until stage 0:** how many consumers silently relied on consensus exporting RocksDB, sqlite3, and `src/daemon` includes, and whether the standalone wallet FFI links today. Stage 0's report gives counts per platform and an estimate per stage; the v8.2.1 decision is made on that report.

Requalification for anything shipped: full Linux/macOS/Windows CI, ASan/UBSan daemon job, compact regtest qualification and vectors, independent vectors and saved proofs, core-heavy, NodeCore rebuild with lifecycle/snapshot consumers, W4/W5 on each platform, and a regtest wallet soak (send, receive, rescan, fee bump, reorg). Consensus logic does not change, but consensus library composition does, so K4 is mandatory on every platform.

## 12. Left to the builder

Exact interface field types; whether chainstate links the wallet after UTXOIndex moves; how K2/W2 walk the closure (CMake script or link-line inspection), as long as they fail on the listed targets; the exact W5 transcript within the listed methods; splitting stages into smaller PRs. **Stop and report** (do not work around) if: any consensus source other than those in 6.1 turns out to need storage or node code; a frozen header would need to change; K4 digests differ; or an external change beyond E1/E2 appears.

## 13. Later legs and follow-ups (listed, not in scope)

**Toward the target architecture:**
- Mempool as a library (`src/daemon/mempool.cpp` is in no library and compiled into ~21 targets); then `dinero_rpc_handlers` declares it, and a forced-link gate for the mempool.
- `dinero_consensus_core` is not self-contained (needs `ScriptVerifier::VerifyTaproot`, `SignatureHashLegacy/Witness/Taproot`, `Params`, `GetActiveChain` from consensus); reconcile the two parallel libraries.
- P2P limited to transport; `dinero_core` → `dinero_rpc_handlers` via `vault_runtime`.
- Whether the process-wide logger is "global node state" under the purity rule.

**Cleanup:**
- Delete uncompiled dead code: `src/mining/miner_engine.*`, `fast_sync_service.cpp`, `methods_snapshot.cpp`, `utreexo_proof_generator.cpp`, `src/daemon/rpc/WalletHandlers.cpp`, `src/core/rpc/wallet_{security,query}_rpc_handlers.cpp`, `src/blockchain/real_blockchain_integration.cpp`, `src/multi_account/multi_account_manager.cpp`, `src/mobile/mobile_wallet{,_simple}.cpp`, `src/wallet/address_minimal.cpp`; then `global_utxo_set_impl` and `coins_db`/`coins_view_cache` if still unused.
- Duplicate objects: `addr_codec.cpp`/`addr_types.cpp` (wallet, core), `block_index.cpp` (consensus, core, 7 tests), `src/wallet/psbt.cpp` (wallet, core).
- `storage/tip_info.h` lives under storage but is pure.
- `FFI_WALLET_ONLY` referenced (`wallet_manager.h:88-94`) but never defined; `BUILD_FFI_ONLY` never exercised in CI.
- `IMempoolInterface` has no production implementer.
- `CrossLanguageVectors` commented out (`tests/consensus/CMakeLists.txt:789`).
- Other workflows pin ubuntu-24.04 without the wallet-link comment.

## Evidence

- `~/src/dinero-evidence/2026-10-28-wallet-node-separation/` — forced-link probe, baseline RED log, `undeclared.json`.
- `~/src/dinero-evidence/2026-10-28-rocksdb-gcc15/` — GCC 15 container and CI link diagnosis.
- PR #856 (merged as 3aca093c0) — the pin this project removes.
