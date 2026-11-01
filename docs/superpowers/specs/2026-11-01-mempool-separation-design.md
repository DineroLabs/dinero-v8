# Mempool as a library (`dinero_mempool`) — design (rev 1.3)

**Status:** owner approved moving to planning (2026-11-01) with decisions D1–D3 recorded in section 14. X1–X5 are proposed in 9.1 and must be resolved jointly with the wallet project before implementation. **Whether Stage 0 (gates and measurement, no product code, but a block in the shared root `CMakeLists.txt` and new test code) counts as implementation is an open owner question (section 14); nothing starts until it is answered.**

**Rev 1.3 changes** (self-review before reporting): the whole planned source set measured, not just `mempool.cpp` (6.1); the logger is a mempool-owned edge, not inherited (section 8, open question L1); 10 more test targets compile moving sources directly (section 9); the consistency trace covers every chain write path, not only the tip (7.4); orphan handling is P2P-only, so MP4 freezes it through non-floor tests, not RPC.

**Rev 1.2 changes** (owner review of rev 1.1): D1–D3 decided; chain-view consistency contract added (7.4); reorg case added to MP4; concrete joint proposals for X1–X4; X5 (gate-script reuse without edits); wording: the library is **not** described as storage-independent while it inherits consensus's storage edges (D1).
**Release:** separate from any release. Whether it ships, and in which version, is decided **after stage 0 measures the scope** (section 11).
**Baseline:** `dinero-main` **c4940d86c** (Merge PR #861, 2026-10-30). All file:line references are against that commit, from a read-only survey on 2026-11-01. No local `v8.2.0` tag exists in this clone; whether the tag equals c4940d86c is checked at stage 0 (the wallet project may re-anchor on the tag; this project follows whatever it anchors on).
**Sibling project:** wallet/node separation (draft PR #860, spec `docs/superpowers/specs/2026-10-29-wallet-node-separation-design.md` on branch `docs/wallet-node-separation-design`). This is its §12 item "Mempool as a library (`src/daemon/mempool.cpp` belongs to no library); `dinero_rpc_handlers` then declares it."

## 1. Target architecture (owner, binding)

From the wallet spec §1, unchanged:

- **Consensus kernel:** deterministic validation, scripts, sighash, subsidies, chain rules.
- **Chainstate:** stores blocks and UTXOs and adapts them to consensus interfaces.
- **Mempool:** owns unconfirmed transactions and policy. **It calls consensus rules; consensus never calls the mempool.**
- **P2P:** transports messages. **Wallet:** keys, accounts, data; node-side adapters. **Daemon:** assembles; `dinero_core` may orchestrate.

This project produces **one library, `dinero_mempool`**, that:
1. contains the mempool and the policy code it owns (admission, RBF/CPFP, fee estimation, persistence, the mempool coins overlay);
2. links on its own (forced, whole-archive) with only its declared dependencies;
3. reaches chain state only through a declared, storage-free chain view;
4. has no edge to `dinero_core`, the wallet, RPC, P2P, gRPC, RocksDB or mining;
5. is declared by its node-side consumers (`dinero_core`, `dinero_rpc_handlers`, and through core `dinerod`, `nodecore_ffi`, tests) instead of being compiled into each one. **`dinero_wallet` does not declare it** (X1): the wallet's M1–M3 edge is removed by the wallet project's own seam, not by linking the mempool into the wallet closure.

## 2. Why now

- **`src/daemon/mempool.cpp` belongs to no library.** It is compiled directly into **57 targets** (identical OFF and ON): `dinerod`, `nodecore_ffi`, `swap_e2e_fixture_daemon`, the OBJECT library `dinero_test_mempool_runtime` (attached to 32 fixtures by `tests/CMakeLists.txt:17204-17309`), and 53 test executables (list: evidence `survey/mempool_cpp_compilers_off.txt`).
- **Two-way dependency with `dinero_core`.** `dinero_core` (20 sources, e.g. `mempool_service.cpp`, `chainstate_service.cpp`, `p2p_service.cpp`, `tx_relay_manager.cpp`, `block_assembler.cpp`) calls `Mempool::` but does not contain it; `mempool.cpp` in turn needs `dinero_core` objects (section 6.1). Both resolve only because the final executable compiles `mempool.cpp` in. `dinero_rpc_handlers` (16 sources) and `dinero_wallet` (5 sources) have the same undeclared edge.
- **Stubs instead of a library.** `tests/stubs/mempool_test_stubs.cpp` replaces six `Mempool` methods with no-ops. 43 live references in `tests/CMakeLists.txt` and 4 in `rust/orchard_backend/CMakeLists.txt:533,544,663,675`; **19 targets** compile it in the surveyed macOS config (OFF and ON identical). The rest are either among the 32 fixtures from which `dinero_attach_real_mempool_fixture_runtime` strips the stubs at configure time (`tests/CMakeLists.txt:17268-17269`, "the former no-op mempool stubs cannot implement those contracts") or gated by `if(UNIX)`, `if(NOT WIN32)`, `DINERO_ENABLE_COMPACT_REGTEST`, `DINERO_TEST_ORCHARD_QT_CONTRACT`, `TARGET gtest`, `TARGET dinero_shielded_prover_kit`; stage 0 enumerates them per platform with those options on. The wallet project's W1/W6 gates forbid the stub; it can't go away while there is no real library to link. The fixtures also link core with `-Wl,--start-group … --end-group` on Linux (`:17280-17284`), i.e. today's resolution depends on link grouping.
- **The wallet project needs it.** Its W2/W6 closures forbid `src/daemon/mempool.cpp`; its §7 mempool seam (M1–M3) is header-only over `Mempool`. A real `dinero_mempool` with a storage-free closure is what lets that seam be linked honestly.

## 3. Goals

1. `dinero_mempool` exists and links on its own (forced, whole-archive) with only declared dependencies.
2. Its **own** sources and direct edges contain no storage code, and its closure has no mempool-owned violation (bucket B, section 8). **It is not described as storage-independent** while its closure still inherits RocksDB/sqlite3 through `dinero_consensus`/`dinero_pq` (bucket A, kept visible in every gate report until wallet Stages 1–3 remove it). Target end state: its transitive closure contains no `dinero_core`, `dinero_chainstate`, `dinero_wallet`, `dinero_rpc_handlers`, P2P, gRPC, RocksDB or sqlite3 code, and no `src/daemon` include directory (except the mempool's own headers, allowlisted, section 6.4).
3. Its chain-state access goes through one declared interface (`IMempoolChainView`, section 7); the ChainDB adapter lives on the node side.
4. **No direct compile of `src/daemon/mempool.cpp` remains** in any target; `dinero_test_mempool_runtime` is removed; consumers link the library.
5. "Consensus never calls the mempool" stays true and mechanically enforced (wallet K2/K3, plus MP2's reverse check).
6. Proven in both Orchard configurations on all four platforms (section 8), with **no behaviour change** (MP4).

## 4. Hard constraints (owner)

- **No behaviour change:** admission policy, RBF/CPFP, fee estimation, eviction/expiry, orphan handling, persistence format (`mempool.dat`, magic `MEMPOOLV`, `VERSION = 1`, `include/mempool/mempool_persistence.h:51-52`), the invalid-tx cache, covenant policy, Orchard mempool entries, P2P relay, RPC methods/fields/errors.
- **External interfaces unchanged:** `include/nodecore/nodecore_ffi.h`, `wallet-core/ffi/wallet_ffi.h`, JNI exports, Qt and NodeCore behaviour.
- **No consensus logic change.** Frozen headers (`docs/consensus_map.md`) are not edited. Nothing under `src/consensus/**` or `include/consensus/**` is edited (wallet project owns them).
- **Wallet's mempool contract is source-compatible:** `Mempool::isOutputSpentInMempool` (M1, `mempool.h:270`), `Mempool::getMempoolEntry` returning `MempoolEntry` whose `tx` is a `MempoolTransaction` (M2, `mempool.h:82`, `:284`), `Mempool::getStats` (M3), and `MempoolTransaction` itself including the Orchard `Historical()` throw (`mempool_transaction.h:75-78`). Any change is a cross-project decision (9.1).
- Internal C++ interfaces may change (the `Mempool` constructor signature, section 7).
- **Moves are surgical.** Files are not moved in this project (section 6.4). Source-list changes are CMake-only. Any line edit inside a mempool source is named in the plan and limited to the include and type swap of section 7; checked with `git diff --stat` per task.
- Any other unavoidable external change is flagged (9.1), never folded in.

## 5. Non-goals and anti-goals

- No change to policy logic, no new RPC, no new FFI.
- **No file moves or renames.** `include/daemon/mempool.h` keeps its path (the wallet's planned `include/daemon/mempool_wallet_view.h` includes it; `include/mempool/mempool.h` is already the legacy `dinero::mempool` header that `check_no_legacy_mempool_prod` keeps out of daemon/p2p).
- Dead code (section 6.3) is left in place, not deleted, not added to the library.
- The legacy `include/mempool/mempool.h` (`dinero::mempool::MempoolAcceptResult`, still included by `methods_wallet_context.cpp:56`, `methods_mempool_context.cpp:31`, `wallet_fee_bump_hooks.cpp:9`, `fee_bump_rpc_handlers.cpp:14`) is left alone.
- The silent-payments scanner is **not** redesigned; it is injected unchanged (section 6.2).
- `IMempoolInterface` (`include/wallet/mempool_interface.h:137`) is not implemented here; whether `Mempool` should implement it, or it is deleted, is a later decision (section 12).
- **Anti-goal:** a gate that passes because of stubs or link order.
- **Anti-goal:** moving a dependency instead of removing it (e.g. putting `ChainDB` or `ScannerManager` into the library).

## 6. Survey at c4940d86c

### 6.1 What `mempool.cpp` needs (measured)

`dinerod`'s `mempool.cpp.o` and `dinero_core`'s objects for the other 11 planned sources (6.3) were compiled (OFF, macOS, AppleClang; ON is measured in plan Task 0.3) and `nm -u -C` taken (evidence `survey/mempool_o_undefined_off.txt`). Every non-std undefined symbol, by defining library:

| Group | Symbols | Defined in (library) | Disposition |
|---|---|---|---|
| Consensus kernel | `ParamsImpl`, `ValidateSpend`, `PrecomputedTransactionData`, `CoinbaseMaturity::isCoinbaseMature`, `FindBlockIndex`, `CBlockIndex::GetMedianTimePast`, `Transaction::*`, `uint256::*` | `dinero_consensus` (also `dinero_consensus_core`, `dinero_tx_primitives`) | **allowed**; declared PUBLIC |
| pq | `consensus::ComputeVWU` | `src/consensus/pq/p2mr_consensus.cpp` → `dinero_pq` | **allowed**; declared |
| Shielded | `shielded::BuildShieldedValidationContext`, `DeserializeShieldedBundle`, `ValidateShieldedBundle` | `dinero_shielded` | **allowed**; declared |
| Chainstate | `ChainDB::getTip` (12 call sites), `getCoinWithConfidentialFallback` (5), `getTxLocation` (2), `forEachUTXO` (2) | `dinero_chainstate` (RocksDB) | **remove**: replaced by `IMempoolChainView` (section 7) |
| Mempool-owned, in core today | `CoinsViewMemPool::*`, `FeeEstimator::*` (`src/mempool/fee_estimator.cpp`), `MempoolPersistence::{load,save}`, `policy::RBFPolicy::*` (`src/policy/rbf_policy.cpp`), `DecodeRelayTransaction` (`src/daemon/relay_transaction_reader.cpp`) | `dinero_core` (`CMakeLists.txt:1043-1049`, `:991-995`, `:1208`) | **move into `dinero_mempool`** (source-list change) |
| Mining (wrong direction) | `mining::CTSelectionPolicy::{CTSelectionPolicy,GetWeightInfo}` (`ct_selection_policy.cpp`, includes only `chainparams.h`); `mining::DecodeAddress`, `mining::BuildScriptPubKey` (`address_validator.cpp`, includes only `bech32_decode.h`, `base58.hpp`) | `dinero_core` (`CMakeLists.txt:1120`, `:1127`) | **move into `dinero_mempool`** as pure policy/codec objects, or a small lower library; builder decides (section 13). Both are storage-free. `mempool.h:25` and `rbf_policy.h:4` include `mining/ct_selection_policy.h` |
| Logger (mempool-owned) | `dinero::g_logger`, `Logger::{debug,info,warning,error}` from `fee_estimator.cpp` (both), `mempool_persistence.cpp`, `tx_orphan_pool.cpp`, `rbf_policy.cpp` and `mempool.h:23` | `src/common/logger.cpp`, compiled into `dinero_core` and `dinero_wallet` | declare the wallet project's `dinero_logging` once its Stage 1 creates it; until then a mempool-owned violation (**L1**, section 8) |
| Codecs | `b58::decode_check`; `mining::Bech32DecodeSegwit` (both from `address_validator.cpp`) | `dinero_crypto` (`src/crypto/base58.cpp`); `dinero_address_encoding` (`src/daemon/bech32_decode.cpp`) | **allowed**; declared. `bech32_decode.cpp` is on the wallet project's pure-codec allowlist; MP2 copies those three entries |
| Serializer | `TransactionSerializer::Deserialize(Transaction&, const vector<uint8_t>&, size_t&)` (from `relay_transaction_reader.cpp`) | `dinero_consensus` (`src/primitives/transaction_serializer.cpp:202`). A second definition in wallet-owned `src/wallet/transaction.cpp:479` compiles in no target | **allowed** |
| Privacy | `din::sp::ScannerManager::{ScannerManager,~ScannerManager,scanMempoolTransaction}`, `acquireEmptyRegistry` (`mempool.cpp:468`, `:793-794`, `:1104-1106`) | `src/core/privacy/silent_scanner_manager.cpp` → `dinero_core` (the copy at `src/privacy/` compiles nowhere) | **inject**: section 6.2 |

Includes of `mempool.cpp` with no remaining symbol use: `daemon/p2p_message.h` (`:11`) shows no `P2PMessage`/`p2p::` reference. Removing it is a named edit, verified by compile, or it is left (section 13).

**Measured result:** the union of undefined symbols of all 12 planned objects, minus what they define themselves, is 38 symbols (evidence `survey/library_set_undefined_off.txt`): the consensus/pq/shielded/codec/serializer rows above (allowed), and **only three foreign groups: ChainDB (4), ScannerManager (3), logger**. Nothing else from core, P2P, RPC, mining or the wallet.

**Logger:** `mempool.cpp` logs through the injected `ILogger` (`MPLOG_*`, `setLogger`); the other sources use the global `g_logger`. That edge comes from the mempool's own sources, not through consensus, so it is **not** bucket A (section 8).

**Globals:** `dinero::Params()` (~15 sites, e.g. `:1030`, `:2232`, `:2734`) and the block-index globals via `FindBlockIndex` (`:3760`, MTP lookup walking `pprev`). Both are consensus-owned process state, on the wallet spec's §12 purity inventory. Allowed; not fixed here.

### 6.2 The scanner edge

`Mempool` owns a `std::unique_ptr<din::sp::ScannerManager>` (`mempool.h:773`, created at `mempool.cpp:468`). It changes behaviour:
- Orchard admission: `if (m_sp_scanner_manager && !scanner) return fail(TxRejectCode::UNAVAILABLE, "Configured scanner cannot consume Orchard body")` (`:793-794`);
- after ordinary admission it scans the tx hex (`:1104-1106`).

Disposition: an injected observer interface, `IMempoolScanSink` (two operations matching today's two uses), owned by `Mempool` exactly as now. `MempoolService` constructs the `ScannerManager`-backed sink and passes it in, so the default daemon state (a scanner always present) is unchanged. MP4 freezes the UNAVAILABLE path and the scan call. The library never includes `privacy/*.h`.

### 6.3 Classification of mempool sources

| Source | Lines | Compiled into (OFF / ON) | Includes beyond consensus/primitives | Disposition |
|---|---|---|---|---|
| `src/daemon/mempool.cpp` | 4,377 | 57 targets, no library | `storage/chain_db.h:10`, `privacy/silent_scanner_manager.h:14`, `mining/address_validator.h:13`, `daemon/p2p_message.h:11`, `daemon/relay_transaction_reader.h:7`, `daemon/utreexo_tx_reader.h:1`, `daemon/tx_mempool.h:12` | **into `dinero_mempool`**; ChainDB and scanner behind interfaces |
| `include/daemon/mempool.h` | 796 | — | `wallet/transaction.h:22`, `common/logger.h:23`, `mining/ct_selection_policy.h:25`, `daemon/connected_block_effects.h`, `daemon/interfaces/ingress_types.h`; forward-declares `ChainDB` (`:59`) | stays; `ChainDB*` ctor parameter replaced (section 7) |
| `include/daemon/mempool_{transaction,acceptance_observer,chainstate_guard}.h`, `tx_mempool.h` (`UTXOView`) | — | header-only | — | stay; part of the library's public headers |
| `src/mempool/coins_view_mempool.cpp`, `fee_estimator.cpp`, `invalid_tx_cache.cpp`, `mempool_persistence.cpp`, `tx_orphan_pool.cpp` | 970 | `dinero_core` (`CMakeLists.txt:1043-1049`); plus tests | consensus `chain_state_view.h`, `daemon/mempool.h` (persistence), `daemon/mempool_transaction.h` (orphan pool) | **into `dinero_mempool`** |
| `src/policy/mempool_policy.cpp`, `rbf_policy.cpp`, `fee_estimator.cpp` | 1,187 | `dinero_core` (`CMakeLists.txt:991-995`); plus tests | `rbf_policy.cpp:3` includes `daemon/mempool.h` | **into `dinero_mempool`** |
| `src/daemon/relay_transaction_reader.cpp` | 35 | `dinero_core` (`:1208`); ON adds `DINERO_HAS_ORCHARD_RELAY_READER=1` via `rust/orchard_backend/CMakeLists.txt:183-184` | `primitives/transaction_reader.h` | **into `dinero_mempool`**; the ON compile definition must follow the source (it is a SOURCE property, so it does) |
| `src/mining/ct_selection_policy.cpp`, `src/mining/address_validator.cpp` | 530 | `dinero_core` | storage-free | **into `dinero_mempool`** or a lower pure library (section 13) |
| `src/daemon/services/mempool_service.cpp` | 354 | `dinero_core` | ChainstateService, P2PService, ConfigService | **stays in core** (it is the node-side assembler); builds the ChainDB view and scanner sink |
| `src/daemon/orchard_mempool_transaction.cpp` (ON) | — | `dinero_orchard_mempool_body` (`rust/orchard_backend/CMakeLists.txt:178-182`), linked PUBLIC by `dinero_core` | `orchard_transaction.h` | stays; `dinero_mempool` links it PUBLIC in ON builds (it defines `MempoolTransaction::FromOrchard`). Its closure (`dinero_transaction_reader` → `dinero_orchard_backend`, `dinero_consensus_core`) is checked by MP2-ON at stage 0 |
| `src/daemon/tx_relay_manager.cpp` | — | `dinero_core` **and** `dinero_test_mempool_runtime` (duplicate) | P2P relay | stays in core (P2P side); the duplicate goes with the OBJECT library |

**Dead — compiled by no target in either configuration (gRPC off):** `src/mempool/covenant_policy.cpp`, `src/mempool/policy_engine.cpp`, `src/policy/hybrid_fee_estimator.cpp`, `src/daemon/{mempool_globals,mempool_events,mempool_stub,validation_mempool,tx_mempool}.cpp`, `src/daemon/rpc/rpc_mempool.cpp`, `src/core/rpc/mempool_rpc_handlers.cpp`, `src/blockchain/real_mempool_integration.cpp`, `src/rpc/methods_mempool_vnext.cpp`, `src/privacy/silent_scanner_manager.cpp`. gRPC-only (`ENABLE_GRPC=ON`, dev mode): `src/grpc/mempool_{service,client}.cpp`, `src/daemon/CMakeLists.txt:13`. Left alone; not in the library. Stale header copies `include/dinero/daemon/mempool.h`, `include/dinero/core/daemon/mempool.h` (153 lines each) likewise.

### 6.4 Paths and the path rule

The wallet spec's path rule (§6.3): any source under `src/{daemon,rpc,storage,p2p,wallet}` inside a storage-free library needs an allowlist entry with a reason. `dinero_mempool` gets its own allowlist, not the wallet's:
- `src/daemon/mempool.cpp` — the mempool itself (path kept, section 5);
- `src/daemon/relay_transaction_reader.cpp` — pure typed decoder;
- `include/daemon/{mempool,mempool_transaction,mempool_acceptance_observer,mempool_chainstate_guard,tx_mempool,connected_block_effects,relay_transaction_reader,utreexo_tx_reader}.h`, `include/daemon/interfaces/ingress_types.h` — header-only or library-owned. Stage 0 confirms each is header-only or in the library. `src/daemon/interfaces/ingress_types.cpp` is compiled into `dinero_core` (and 9 tests); `mempool.o` has no undefined symbol from it, so it stays in core unless the MP1 link shows otherwise.

- `src/daemon/bech32_decode.cpp`, `src/daemon/bech32_encoder.cpp`, `include/daemon/bech32_decode.h` — the wallet project's three pure-codec entries, copied verbatim with their reason, because `address_validator.cpp` reaches them through `dinero_address_encoding` (6.1).

The `src/daemon` **include directory** itself is not exported by `dinero_mempool`; consumers already add `include/`.

### 6.5 Who calls the mempool

Production sources referencing `Mempool`/`MempoolEntry`/`MempoolTransaction`/`MempoolService` by target (ON; evidence `survey/mempool_users_by_target_on.txt`):

| Target | Sources | Through |
|---|---|---|
| `dinero_core` | 20 (`chainstate_service`, `mempool_service`, `p2p_service`, `rpc_service`, `mining_service`, `swap_service`, `block_relay_manager`, `tx_relay_manager`, `transaction_pool`, `reopen_candidate_journal`, `relay_transaction_reader`, `compact_block`, `block_assembler`, `block_assembly_context`, `mining_manager_v2`, `transaction_scorer`, `tx_inclusion_analyzer`, + 3 moving in) | `Mempool&` via `MempoolAccess::Pool()` (`daemon/interfaces/mempool_access.h:14`), `MempoolService`, direct pointers |
| `dinero_rpc_handlers` | 16 (`methods_mempool_context`, `methods_wallet_context`, `fee_bump_rpc_handlers`, `methods_mining_*`, `orchard_account_rpc`, `websocket_server`, …) | `Mempool&` |
| `dinero_wallet` | 5 (`wallet_manager`, `rbf_cpfp_detector`, `fee_bump_engine`, `slow_reason_analyzer`, `wallet_fee_bump_hooks`) | M1–M3 (wallet spec §7.1) |
| `dinerod`, `nodecore_ffi`, `swap_e2e_fixture_daemon` | `daemon_app.cpp` (persistence `loadFromDisk`/`saveToDisk` at `:7459`, `:7981`), `http_rpc_server.cpp`, `nodecore_ffi.cpp` | direct |
| `dinero_orchard_mempool_body` (ON) | `orchard_mempool_transaction.cpp` | defines `MempoolTransaction::FromOrchard` |

**Construction:** one production site, `mempool_service.cpp:125` (`std::make_unique<Mempool>(chain_db, chainstate_->GetConsensusUTXOSet(), utreexo_stateless)`), plus injected callbacks already in place: `setChainstateReadGuardFactory`, `setPreBaseCoinResolver`, `setPreBaseCoinStatusResolver`, `setPreBaseCoinPredicate` (`:131-153`). **~46 test files** construct a `Mempool` directly (regex count; exact list at stage 0).

### 6.6 Consensus never calls the mempool (checked)

Matches of `Mempool` under consensus, classified: `header_store.h:54`, `header_sync.h:17`, `chainparams.h:117`, `header_chain.h:172`, `script_validation.h:76`, `transaction_validator.cpp:744` are comments; `iconsensus_engine.h:16` is a bare `class Mempool;` forward declaration with no use. No consensus source calls the mempool. The wallet project's K2 (forbids any target compiling `src/daemon/mempool.cpp` in the consensus closure) and K3 (forbids `dinero::Mempool` symbols) enforce it; **they key on the path and symbol prefix, so they stay valid because the file is not moved** — but K2 must also forbid the new target name `dinero_mempool` (9.1, X2).

### 6.7 Library shape

```
dinero_mempool  STATIC   (new; root CMakeLists.txt, placed before add_library(dinero_core))
  sources   src/daemon/mempool.cpp
            src/mempool/{coins_view_mempool,fee_estimator,invalid_tx_cache,
                         mempool_persistence,tx_orphan_pool}.cpp
            src/policy/{mempool_policy,rbf_policy,fee_estimator}.cpp
            src/daemon/relay_transaction_reader.cpp
            src/mining/{ct_selection_policy,address_validator}.cpp   (or a lower lib, §13)
  PUBLIC    dinero_consensus  dinero_pq  dinero_shielded  dinero_tx_primitives  dinero_crypto
            dinero_address_encoding (bech32, from address_validator)
            dinero_logging (once wallet Stage 1 creates it; until then L1, section 8)
            ON only: dinero_orchard_mempool_body
  includes  ${CMAKE_SOURCE_DIR}/include  (no src/daemon, no RocksDB dir)
  headers   include/daemon/mempool*.h, tx_mempool.h, mempool_chain_view.h (new),
            mempool_scan_sink.h (new), include/mempool/*.h (non-legacy), include/policy/*.h
node side   include/storage/chain_db_mempool_view.h   header-only ChainDbMempoolView
            core: ScannerManager-backed IMempoolScanSink (in mempool_service.cpp or beside it)
consumers   dinero_core PUBLIC dinero_mempool;  dinero_rpc_handlers declares it
ON hook     rust/orchard_backend/CMakeLists.txt:181-184 gains
            if(TARGET dinero_mempool) target_link_libraries(dinero_mempool PUBLIC dinero_orchard_mempool_body)
            (the relay-reader COMPILE_DEFINITIONS is a SOURCE property, so it follows the file)
```

Exact PUBLIC set is what MP1 proves necessary; anything not needed is dropped (stage 3).

## 7. Interfaces

### 7.1 `IMempoolChainView` (replaces `ChainDB*`)

Exactly the measured ChainDB call set, nothing more:

```cpp
// include/daemon/mempool_chain_view.h  (library-owned, storage-free)
struct MempoolChainTip { int32_t height; uint256 hash; };
class IMempoolChainView {
public:
    virtual ~IMempoolChainView() = default;
    virtual StatusOr<MempoolChainTip> GetTip() const = 0;                       // getTip: height, hash
    virtual StatusOr<Coin> GetCoinWithConfidentialFallback(const uint256& txid,
                                                           uint32_t vout) const = 0;
    virtual bool HaveTransaction(const uint256& txid) const = 0;                // getTxLocation().ok()
    virtual bool HaveConfidentialCommitment(const std::vector<uint8_t>&) const = 0;
        // replaces the two forEachUTXO scans (mempool.cpp:174, :371); same predicate
        // (is_confidential && commitment ==), same early exit
};
```

- Field and status types are copied from ChainDB's. **`Coin` is defined only in `include/storage/chain_db.h:37`**, which pulls RocksDB headers, so the view cannot return it as is (X4): either a field-for-field `MempoolCoin` in a library header, or `consensus::UTXOEntry` (which already carries `is_confidential` and `commitment`, see `mempool_transaction.h` `OutputCoin`) if every read field maps one-to-one. Stage 0 lists the `Coin` fields `mempool.cpp` reads.
- `Mempool(ChainDB*, const IConsensusUTXOSet*, bool)` becomes `Mempool(const IMempoolChainView*, const IConsensusUTXOSet*, bool)`. `nullptr` keeps today's `if (!chain_db_)` behaviour at every site.
- The three in-file ChainDB adapters (`ChainDBUTXOView` `:108`, `ChainDBStateView` `:187`, the `MempoolUTXOView` member `:281`) switch from `ChainDB*` to `const IMempoolChainView*`. These are **the only logic-adjacent edits in `mempool.cpp`**, of two kinds, each named in the plan:
  - a type and include swap where the call maps one-to-one (`getTip`, `getCoinWithConfidentialFallback`, `getTxLocation`);
  - a **relocation**: the two `forEachUTXO` commitment scans (`:169-181`, `:366-375`) move verbatim into `ChainDbMempoolView::HaveConfidentialCommitment`, and the call sites call it. Same predicate, same early exit.
- **Node adapter:** `include/storage/chain_db_mempool_view.h`, header-only, `ChainDbMempoolView : IMempoolChainView`, forwarding each call to ChainDB unchanged. `MempoolService` and tests include it; they already link chainstate. Same pattern as the wallet's `chain_db_wallet_view.h`.

### 7.2 `IMempoolScanSink` (replaces the owned `ScannerManager`)

```cpp
class IMempoolScanSink {
public:
    struct Guard { virtual ~Guard() = default; };            // opaque; holds the scanner lock
    virtual ~IMempoolScanSink() = default;
    virtual std::unique_ptr<Guard> AcquireEmptyRegistry() = 0; // null = scanners registered
    virtual void ScanMempoolTransaction(const std::string& tx_hex) = 0;
};
```

`ScannerManager::acquireEmptyRegistry()` (`include/privacy/silent_scanner_manager.h:32-35`) returns a `unique_ptr<EmptyRegistry>` that **holds `scanners_mutex_` for its lifetime**, and `mempool.cpp:793` keeps it alive through the rest of Orchard admission. The interface therefore returns an RAII guard, not a bool; the core implementation wraps `EmptyRegistry` in it, so lock scope and ordering are unchanged. Owned by `Mempool` as `std::unique_ptr` (same lifetime as today). The `ScannerManager`-backed implementation lives in core.

**Today the mempool's manager is always empty.** `m_sp_scanner_manager` is private (`mempool.h:773`), has no accessor, and nothing calls `addScanner` on it (the only uses are `mempool.cpp:468`, `:793-794`, `:1104-1106`). So `acquireEmptyRegistry()` always succeeds, the "Configured scanner cannot consume Orchard body" rejection is **unreachable in production**, and `scanMempoolTransaction` parses the hex and matches zero scanners. Consequences:
- The library needs no default `ScannerManager`. A **null sink** means "no scanners registered" and is observably equivalent to today's empty manager, **except** that it takes no `scanners_mutex_` and skips the parse; neither is visible through RPC, persistence or relay.
- To stay strictly equal, `MempoolService` still passes a `ScannerManager`-backed sink (lock and parse kept in the daemon); tests that construct `Mempool` directly may pass none.
- MP4 freezes the observable path (Orchard entry accepted). The unreachable UNAVAILABLE branch is kept verbatim and covered by one unit test with a fake sink returning null.

### 7.4 Consistency contract for the chain view

The owner's concern: a narrow interface is safe only if its answers stay coherent while the tip changes. The contract below **is today's contract**, written down; this project must not weaken it and does not strengthen it (that would be a behaviour change).

**Per call.** Each `IMempoolChainView` call is answered by exactly one ChainDB call: `GetTip` → `ChainDB::getTip` (one RocksDB `Get` of `KEY_TIP`, no snapshot, `chain_db.cpp:1101-1125`); `GetCoinWithConfidentialFallback` → the same-named ChainDB call; `HaveTransaction` → `getTxLocation().ok()`; `HaveConfidentialCommitment` → one `forEachUTXO` iteration with the moved lambda. The adapter adds **no cache, no snapshot, no lock and no retry**. ChainDB gives **no cross-call snapshot** today; neither does the view.

**Across calls: only the external read guard gives coherence.** `MempoolChainstateReadGuard` (`mempool_chainstate_guard.h:30`; implementation `ChainstateService::MempoolReadGuard`, `chainstate_service.cpp:20079`) holds `snapshot_load_mutex_` and the block-ingress activation lock for its lifetime, and is taken **before** the pool lock `m_mutex`. Five functions take it: `submitTransactionInternal` (`mempool.cpp:873`), `submitOrchardBody` (`:787`), `getConfirmedWalletCoins` (`:1840`), `CaptureEntriesWithInputCoins` (`:2003`), `CaptureBlockSelectionImpl` (`:2197`). View calls made inside those scopes see one coherent chain state **provided every chain write — tip, coins and tx index alike — happens under the activation lock**. A guarded scope reads all three, so a coin or tx-index write outside the lock would make two reads disagree even with a fixed tip. Every ChainDB write takes a `ChainWriteToken` (`include/storage/chain_write_token.h`), which only its friends can mint (BlockAcceptor, PruneService, BlockReindexer, HeaderSyncManager, UTXOIndex, `ChainDB::init`, `InitializeGenesis`). Stage 0 traces each minting path — including AssumeUTXO import and snapshot load — to the lock it holds when writing (plan Task 0.2). Any writer that does not hold it is a pre-existing defect: **stop and report**; it is not fixed here.

**Stage 0 result (2026-11-01, `~/src/dinero-evidence/2026-11-01-mempool-separation/stage0/chain-writers.md`):** all live ChainDB tip and coin writers **found so far** hold the lock (ConnectTip, DisconnectTip, ActivateBestChainPass, InvalidateBlock, the Orchard RPC block path, promotion, background validation); the one unlocked tip/coin writer, `BlockAcceptor::ApplyTipInvalidation`, is reachable only from a source no target compiles. **Open gaps:** mutators of the in-memory `IConsensusUTXOSet` (the mempool's primary coin source; this contract covers ChainDB-backed reads, and that input is unchanged by this project) are traced only for the two reload paths; "startup only" writers are inferred, not proven against service start order. **One live tx-index writer does not: F2, `RebuildTxIndex()` (`chainstate_service.cpp:13527`), reachable via RPC `reindextx` (`methods_address_index.cpp:1065`).** While it runs, `HaveTransaction` can change between two reads in a guarded scope. Also **F3:** `ChainWriteToken::CreateForTesting()` is public and used by production code at 16 sites, so the token does not restrict writers; the trace covered those sites. Both are pre-existing and outside this project. **F2 does not block this project** (the view is a pass-through; separation neither causes nor worsens it) but needs its own fix decision: take the ingress use or activation lock in `RebuildTxIndex`.

**Paths that read the chain without the guard today** (unchanged by this project, listed so nobody assumes otherwise):

| Path | Chain reads | Entered from |
|---|---|---|
| `submitTransactionTestOnly` (`mempool.cpp:1536`) | `getTip` | `testmempoolaccept` (`methods_mempool_context.cpp:1176`), `MempoolService` (`mempool_service.h:144`) |
| `removeConfirmedTransactions` (`:2943`) | `getTip` | `rpc_server.cpp:2140` |
| `validateTransaction` (`:3538`, `:3580`, `:3700`, `:3758`, `:3803`) | `getTip`, coin lookups, MTP walk via `FindBlockIndex` | guarded when reached from `submitTransactionInternal` (`:899`); unguarded when reached from the test-only path |
| `isSelectableAtHeightLocked` (`:2829`), `computeVWUForTx` (`:3904`) | coin lookups via `MempoolUTXOView` | guarded from `CaptureBlockSelectionImpl`; `computeVWUForTx` also from `:1441`, `:2665` |

On these paths two view calls can straddle a tip change, exactly as two ChainDB calls can today. This is recorded as finding **F1** (separate decision, section 12), not fixed here.

**How the contract is held mechanically (not by review alone):**
- **Call-site identity.** A script lists, per enclosing function, the multiset of chain reads before (ChainDB calls) and after (view calls). Expected: identical. 21 + 2 fallback sites at c4940d86c.
- **Guard placement unchanged.** The same script lists the five guard acquisitions and their position relative to `m_mutex` acquisition. Expected: identical.
- **No adapter state.** `ChainDbMempoolView` has exactly one data member, `const ChainDB*`; a compile-time test (`static_assert(sizeof(ChainDbMempoolView) == sizeof(void*) * 2)`, vptr + pointer) fails if anyone adds a cache.
- **Lock order.** The adapter takes no lock, so the order guard → `m_mutex` → ChainDB-internal is unchanged; the call-site script also fails if any view method body acquires a mutex.

**Lifetime.** Non-owning `const IMempoolChainView*`, as `ChainDB*` is today (`mempool.h:757`). `MempoolService` owns the `ChainDbMempoolView` as a member declared **before** `mempool_`, so it is destroyed after the pool; the `ChainDB` it points to is owned by `ChainstateService`, which must already outlive the pool today. Tests declare the view before the `Mempool`. A null view keeps every `if (!chain_db_)` branch's behaviour.

**Reorg.** The mempool learns about reorgs only through existing calls from ChainstateService (`onBlockConnected`/`onBlockDisconnected`, `PreparedBlockUpdate`, `ReconcileAfterReorg` at `chainstate_service.cpp:13172`, `:16917`), made under chainstate's own lock scope. The view has no notification role and holds no state, so reorg behaviour cannot change through it. MP4 adds a reorg case (section 8).

**Side effect kept.** `ChainDB::forEachUTXO` appends to `/tmp/chaindb_foreach_utxo.log` (`chain_db.cpp:3086`). The relocated scan still calls `ChainDB::forEachUTXO`, so the side effect is unchanged (its removal would be a separate decision).

### 7.3 Unchanged

`isOutputSpentInMempool`, `getMempoolEntry`, `getStats`, `MempoolEntry`, `MempoolTransaction`, `UTXOView`, `MempoolAccess`, `ITxIngress`, all observer and guard types.

## 8. Gates (written first, observed on the baseline)

Each gate is added at stage 0 as a non-required job; its baseline result is recorded. Every gate runs with `DINERO_BUILD_ORCHARD_BACKEND=OFF` and `=ON`, on Linux GCC 15 (ubuntu-latest), Linux GCC 13 (ubuntu-24.04), macOS (macos-15, Xcode 16.4) and Windows MSVC.

**Two buckets.** At c4940d86c `dinero_consensus` itself still carries RocksDB, sqlite3, the `src/daemon` include dir and pq's wallet/RPC sources, and `dinero_logging` doesn't exist (wallet K1/K2 RED; #860). `dinero_mempool` must link consensus, so its closure inherits those violations. MP1–MP3 therefore report:
- **Bucket A — inherited:** reachable only through `dinero_consensus` or `dinero_pq`. Owned by wallet K2; reported, not failing, until K2 is green (D1).
- **L1 — logger (mempool-owned, blocked):** the `g_logger` edge from the mempool's own sources. It can't be declared until the wallet project's Stage 1 creates `dinero_logging`. **Proposal, needs owner confirmation because it extends D1:** report it on its own row, not failing, until `dinero_logging` exists; then it must be declared at once and becomes bucket B.
- **Bucket B — mempool-owned:** everything else. Must be zero for the gate to pass.

Stage exits (section 10) require bucket B = 0; full green (A and B) follows wallet Stages 1–3. **Owner decision D1.**

**RED for the right reason.** "Target does not exist" is not a meaningful RED. Stage 0 adds an `EXCLUDE_FROM_ALL` **probe archive**, `sep_mempool_probe`, built at the baseline from the planned source list (6.3) with the planned declared deps. MP1/MP3/MP5 run against it at baseline, and MP2 walks it. Expected baseline RED, from the measured `nm -u` (6.1):
- MP1/MP5 link: undefined `ChainDB::{getTip, getCoinWithConfidentialFallback, getTxLocation, forEachUTXO}`, `din::sp::ScannerManager::{ScannerManager, ~ScannerManager, scanMempoolTransaction}` (and `acquireEmptyRegistry` if out-of-line), plus bucket A;
- MP2: `dinero_chainstate`/RocksDB needed for ChainDB, `dinero_core` needed for the scanner;
- MP3: `ChainDB`, `din::sp::` in the probe's link;
- MP4: GREEN.
The probe becomes `dinero_mempool` at stage 2.

| Gate | What | Baseline expectation |
|---|---|---|
| **MP1 forced link** | Executable linking `dinero_mempool` whole-archive (`--whole-archive` / `-force_load` / `/WHOLEARCHIVE`) plus only its declared deps, `--no-undefined` semantics. `main` constructs a `Mempool` on a fake `IMempoolChainView` and calls `size()` | RED on `sep_mempool_probe`: undefined `ChainDB::*`, `ScannerManager::*`, plus bucket A |
| **MP2 closure** | Walk `dinero_mempool` usage requirements recursively (incl. PRIVATE of static libs). **Allowed:** `dinero_consensus`, `dinero_consensus_core`, `dinero_tx_primitives`, `dinero_pq` (kernel), `dinero_shielded`, `dinero_crypto`, `dinero_logging`, and in ON `dinero_orchard_mempool_body` and its closure. **Forbidden:** `dinero_core`, `dinero_chainstate`, `dinero_wallet`, `dinero_rpc_handlers`, P2P targets, gRPC, RocksDB, sqlite3, `src/daemon` include dirs, any target compiling `tests/stubs/mempool_test_stubs.cpp`, cycles in the closure, non-allowlisted node-path sources (6.4). **Reverse check:** no target in the `dinero_consensus` closure links `dinero_mempool` | RED on the probe: chainstate/RocksDB and core edges |
| **MP3 symbols** | `nm -C` on MP1's output: no `rocksdb::`, `sqlite3_`, `ChainDB`, `ChainstateService`, `din::sp::`, `PeerManager`, `P2PService`, `RPCServer`, `WalletManager`, `grpc::` | RED |
| **MP4 behaviour freeze** | (a) **RPC transcript** on regtest, field for field (timestamps and other run-varying fields masked by an explicit, reviewed list): `getmempoolinfo`; `getrawmempool` (plain and verbose); `getmempoolentry`; `getmempoolancestors`/`descendants`; `testmempoolaccept` (accept, reject, duplicate); `sendrawtransaction`; an RBF replacement (accepted and rejected); a CPFP child; orphans are **not** reachable over RPC — `TxOrphanPool` is owned by `DaemonApp` (`daemon_app.cpp:2779-2782`) and fed only by P2P relay (`tx_relay_manager.cpp:10`), never by `Mempool` — so orphan handling is frozen by running the existing non-floor tests `TxRelayG3Verification`, `RelayRefreshOwner`, `CsnProofRefresh` (checked against the floor list) before and after, with `--no-tests=error` and log grep; a **reorg**: `invalidateblock` on a block holding a mempool-spent tx → the disconnected tx returns to the pool (`ReconcileAfterReorg`, `chainstate_service.cpp:13172`), `reconsiderblock` → it leaves again, with `getrawmempool true` captured at each step; `estimatesmartfee`/`estimatefee`; three error cases; in ON builds an Orchard mempool entry (accepted, and the scanner-UNAVAILABLE path if reachable over RPC, else by a unit test). (b) **Persistence, inside the same regtest run:** populate, `stop` (save), restart (load), compare. Facts that shape it: `m_transactions` is an `unordered_map` (`mempool.h:723`) and `saveToDisk` writes in its iteration order (`mempool.cpp:4204-4209`), so entry order is not stable across runs or standard libraries; and `loadFromDisk` **re-admits every body against the current tip** (`:4254-4280`, "disk metadata never grants admission"), so a `mempool.dat` without its chain would be rejected wholesale and a round-trip would compare near-empty files. Therefore: (i) **format identity** — header (magic `MEMPOOLV`, `VERSION`) byte-identical, and the decoded entry set sorted by txid identical (tx bytes exact; any run-varying field on the reviewed mask list); (ii) **reload identity** — `getrawmempool true` after restart equals before restart, masked the same way, and equals the baseline build's own run; (iii) ON: the set contains an Orchard entry (load decodes `AvailableFamilies`, `:4242`). The reference is the c4940d86c build's output on the same platform and configuration | GREEN (reference run) |
| **MP5 standalone consumer** | `sep_mempool_standalone`, linked only through `target_link_libraries(... dinero_mempool)` (no `dinero_core`, no stubs, no hand-listed archives). On a fake chain view it must **run**: submit a valid tx (accepted), an invalid one (rejected with the frozen reason), save, clear, load, compare. Its resolved closure must pass MP2's rules | RED on the probe (as MP1) |

**MP4 and the never-run floor.** `MempoolPolicyOwner`, `MempoolTestAcceptReadOnly`, `BlockRelayPoolOwner`, `CsnCpfpDisconnect`, `AddressBalanceMempoolOverlay`, `UtreexoMempoolCanonicalSeparation` and other mempool-adjacent tests are on the floor (`scripts/ci/separation/never_run_floor.txt` on #860's branch, 230 names). MP4 uses its own transcript and fixture checks and runs **no** floor test; the gate script refuses any name on the floor list.

**Known baseline conditions** (latest: #860 run 4, `~/src/dinero-evidence/2026-10-29-separation-stage0/ci-pr860-run4/SUMMARY.md:34-39`):
- Windows OFF: the wallet's regtest transcript (W5) is **UNMEASURED (B1, `stol` overflow)** → MP4(a) on Windows is expected UNMEASURED at baseline for the same harness cause; recorded, not hidden. (Run 1 showed `dinerod.exe` rc 1 at startup; run 4 names B1.)
- Windows ON: `dinerod` fails to build, **B3 `LNK2005 CoinbaseMaturity::isCoinbaseMature`** (`wallet_manager.cpp.obj` vs `coinbase_maturity.cpp.obj`) → every ON gate that needs `dinerod` is UNMEASURED on MSVC until B3 is fixed.
- `nodecore_ffi` is not an MSVC target (owner, 2026-10-30).

## 9. Affected consumers

| Consumer | Change | Behaviour |
|---|---|---|
| `dinero_core` | loses the 8 mempool-owned sources + `ct_selection_policy`, `address_validator` (if they move); links `dinero_mempool` PUBLIC; `MempoolService` builds `ChainDbMempoolView` and the scanner sink | unchanged; MP4 |
| `dinero_rpc_handlers` | declares `dinero_mempool` | unchanged |
| `dinero_wallet` | **no change** (X1): does not declare `dinero_mempool`; its 5 M1–M3 sources keep resolving at the final link exactly as today, until the wallet project's seam (its Stage 5) removes the edge | unchanged |
| `dinerod` (both `add_executable` branches, `src/daemon/CMakeLists.txt:58`, `:133`), `nodecore_ffi` (`src/nodecore/CMakeLists.txt:50`), `swap_e2e_fixture_daemon` | drop the direct `mempool.cpp`; link the library (via core). The NodeCore xcframework script bundles `libdinero_mempool.a` | unchanged; W5 (wallet) and MP4 |
| Orchard backend (ON) | `dinero_mempool` links `dinero_orchard_mempool_body`; 4 stub users at `rust/orchard_backend/CMakeLists.txt:533,544,663,675` re-decided with X3 | unchanged |
| Tests | 53 direct compiles of `mempool.cpp` and `dinero_test_mempool_runtime` (32 fixtures) replaced by linking `dinero_mempool`; **10 more test targets compile other moving sources directly** and must drop them, or they duplicate the library's objects (link-order anti-goal; LNK2005 on MSVC): `test_mempool_proof_staleness` (coins_view_mempool, address_validator), `test_fee_estimation_g4`, `test_csn_proof_refresh`, `test_relay_refresh_owner`, `test_tx_relay_g3` (orphan pool), `test_mempool_policy`, `test_rbf_policy_service`, `test_ct_fee_policy`, `test_ct_selection_policy`, `test_mempool_address_queries` (evidence `survey/moving_sources_direct_compiles.txt`, identical OFF/ON); 46 test files that construct `Mempool(ChainDB*,…)` pass `ChainDbMempoolView` (mechanical). Duplicate `tx_relay_manager.cpp` compile goes with the OBJECT library | unchanged |
| Tests using `mempool_test_stubs.cpp` (19 configured; 43 + 4 refs) | **X3** — they define `Mempool::` methods that the real archive also defines; any target that links `dinero_mempool` (via core) and the stubs gets duplicate definitions (LNK2005 on MSVC, link-order luck elsewhere). Each must either drop the stubs and link the library, or not link core | unchanged |

**Hot shared files** (both projects edit; list for merge ordering, section 10):
- root `CMakeLists.txt`: blocks `:991-995` (policy sources), `:1043-1049` (mempool support), `:1102`/`:1120`/`:1127`/`:1208` (single lines in `dinero_core`'s list), one new `add_library(dinero_mempool …)` block placed **after** `dinero_core`'s source sets and before `add_library(dinero_core` (`:1063`), and one `target_link_libraries(dinero_core PUBLIC dinero_mempool)` line next to core's existing link block (`:1253`ff).
- `tests/CMakeLists.txt`: the 50 direct `mempool.cpp` entries (52 refs, 2 commented), the `dinero_attach_real_mempool_fixture_runtime` function (`:17204-17309`), and the stub entries only as X3 decides.
- `rust/orchard_backend/CMakeLists.txt`: `:180-184` (body link, relay-reader property), `:533,544,663,675` (stubs, per X3).
- Also `tests/mining/CMakeLists.txt:147,195,374,461`, `src/daemon/CMakeLists.txt:58,133`, `src/nodecore/CMakeLists.txt:50`.

### 9.1 Flagged for decision (not folded in)

**Cross-project — resolve jointly before Stage 1 (owner decision 2026-11-01). Proposals:**
- **X1. Wallet's mempool edge.** `dinero_wallet` uses M1–M3 today. Until the wallet's `IWalletMempoolView` seam lands, linking `dinero_mempool` from the wallet would put the mempool **inside** W2/W6's closure, which they forbid. **Proposal:** the wallet does **not** declare `dinero_mempool`; its 5 sources keep resolving through the final link as today, and the wallet project's Stage 5 (Task 5.3) removes the edge. This project adds no wallet edit. *Joint check:* the wallet's W1 baseline RED list keeps its mempool symbols until its Task 5.3; nothing in this project changes that list.
- **X2. Wallet gate rules must learn the new target.** K2/W2/W6 forbid the *path* `src/daemon/mempool.cpp`; after this project they should also forbid the *target* `dinero_mempool`. K3/W3 forbid symbol prefix `dinero::Mempool`, which also matches `dinero::MempoolTransaction`/`MempoolEntry` — relevant if `WalletMempoolEntry` wraps those types. The rules file is wallet-owned; proposed text only. **Proposal:** add `{"label": "dinero_mempool", "names": ["dinero_mempool"], "libs": []}` to `forbidden_targets` of K2, W2 and W6 in `closure_rules.json`, in the wallet project's own commit, landing with or before this project's Stage 2; keep the K3/W3 prefix `dinero::Mempool` as is, which requires that `WalletMempoolEntry` copies fields and never holds a `MempoolTransaction`/`MempoolEntry` (consistent with wallet spec §7.2: fee, tx_size, depends, historical inputs).
- **X3. `mempool_test_stubs.cpp`.** Used by 19+4 targets and by the wallet project's `tests/separation/CMakeLists.txt:77-80` (W4 checker). Once core links the real library, stubs plus core is an ODR violation. Options: (a) each stub user drops core; (b) each drops the stub and links `dinero_mempool`; (c) stubs stay only where neither core nor `dinero_mempool` is linked. Needs a joint decision because W4/W6 depend on it. **Proposal: a rule, not per-target choices — "a target may compile `mempool_test_stubs.cpp` only if its link closure contains neither `dinero_core` nor `dinero_mempool`."** Stub users that link core (directly or via the fixture runtime) drop the stub and get the real library through core, as the 32 fixtures already do (`tests/CMakeLists.txt:17268-17269`); the rest keep it. The wallet's W4 checker links `dinero_wallet` only, and per X1 the wallet doesn't link `dinero_mempool`, so **W4 keeps its stub unchanged** until the wallet's Task 5.3 removes the need. The rule is enforced by MP2 (forbidden: any target in a closure containing `dinero_mempool` that compiles the stub) and checked over all targets in the codemodel, not just MP roots.
- **X4. `Coin` type location** for `IMempoolChainView` (7.1). `Coin` is only in `storage/chain_db.h:37`, so a storage-free copy or a `consensus::UTXOEntry`-based return is needed. The wallet's planned `IWalletChainView` may hit the same type; consensus headers are wallet-owned, so any new header goes under `include/daemon/` or `include/mempool/` and both projects should agree on one. **Proposal:** move `struct Coin` (`chain_db.h:37-44`, 6 fields, std types only) **unchanged** into a new header-only `include/storage/coin.h` (std includes only), and make `chain_db.h` include it in place of the definition. Same name, same namespace, same layout: every existing user compiles unchanged. A conversion to `consensus::UTXOEntry` is rejected: `Coin` stores `script_pubkey` as a hex `std::string` and `height` as signed `int`, while `UTXOEntry` uses bytes and `uint32_t`, so a conversion is not one-to-one (the hex parse and negative heights would be new behaviour). `include/storage/coin.h` is a header, not a source, so the path rule (6.4) is unaffected; `storage/tip_info.h` is the precedent. The header is shared: the wallet project uses it if its chain view needs `Coin`. `chain_db.h` is neither wallet- nor consensus-owned; the edit is one contiguous block.
- **X5. Reusing #860's gate scripts without editing them (D2).** `check_link_closure.py --gate` accepts only K2/W2/W6 and `check_symbols.py --gate` only K3/W3 (their argparse `choices`). This project adds thin wrappers under `scripts/ci/mempool-separation/` that **import** `Model`, `check`, `load_json` and `parse`/`symbols`/`nm_text` and apply mempool rules; a pytest pins the imported names and signatures so a wallet-side refactor fails loudly. No wallet-owned file is edited. If the wallet project prefers, a later change on its side can accept arbitrary gate keys and the wrappers shrink to CLI calls.

**Separate decisions:**
- **E1.** Whether `Mempool` should implement `IMempoolInterface` or that interface is deleted (section 12).
- **E2.** `check_no_legacy_mempool_prod` is a custom target **no workflow or dependency runs** (PASS when run by hand at c4940d86c). Wiring it into CI is a separate decision; this project keeps it passing (MP2 adds no `mempool::Mempool` use).

## 10. Stages

Each stage is its own PR onto `dinero-main`, green on existing CI plus the named gates, in both Orchard configurations, before the next starts. Gates observed failing before the change that turns them green; logs grepped to show tests ran (`--no-tests=error`); touched sources rebuilt from clean.

| Stage | Content | Exit |
|---|---|---|
| **0 — refresh, gates, measurement** | Refresh baseline; re-check every cited line. Add MP1–MP5 (non-required), the MP4 transcripts and per-config `mempool.dat` fixtures, the closure rules and allowlist. **Measurement** on a throwaway branch: apply stages 1–3 in one go, build every target on every platform in both configs, record what breaks and why | MP1/MP2/MP3/MP5 RED as expected; MP4 GREEN (or UNMEASURED with cause); breakage list recorded → **owner decides release fit** |
| 1 | Interfaces: `IMempoolChainView`, `IMempoolScanSink`, `ChainDbMempoolView`; `Mempool` ctor switch; `MempoolService` and the 46 test sites pass the adapter. **No CMake library yet** | MP4 GREEN; full CI; `mempool.o` has no `ChainDB::`/`din::sp::` undefined refs (nm check) |
| 2 | `add_library(dinero_mempool)` with the sources of 6.3; `dinero_core`, `dinero_rpc_handlers` declare it; remove all 57 direct compiles and `dinero_test_mempool_runtime`; stub users per X3 | MP1, MP3, MP5 GREEN for bucket B; MP4 GREEN; full CI on all platforms, both configs |
| 3 | Closure cleanup: library exports only what it uses; consumers declare their own deps where they relied on core exporting them | MP2 bucket B = 0 everywhere |
| 4 | Gates become required (bucket B). Bucket A turns into a hard failure after wallet Stages 1–3 land | MP1–MP5 fully GREEN |

**Ordering with the wallet project.** Stages 1 and 2 touch the hot files listed in section 9; they should land **after** #860's Stage 0 and **before** the wallet's Stage 5 (which consumes the mempool via its seam). The owner orders the merges.

## 11. Scope measurement

**Known now:**
- 1 core file to switch (`mempool.cpp`, three in-file adapters, 21 ChainDB call sites, 3 scanner sites);
- 10 sources move between CMake lists (8 mempool-owned + 2 mining, if they move);
- 57 direct-compile targets + 1 OBJECT library (32 fixtures);
- 46 test files constructing `Mempool`;
- 19 configured stub users (43 + 4 references);
- 1 production constructor site.

**Unknown until stage 0:**
- what `dinero_core`, `dinero_rpc_handlers` and the tests relied on core exporting;
- the ON-build closure of `dinero_orchard_mempool_body`;
- MSVC behaviour (duplicate definitions, `ParamsImpl` LNK2005 history, root `CMakeLists.txt:520-534`);
- whether `Coin` is storage-free (X4) and `acquireEmptyRegistry` lifetime (7.2).

**Requalification for anything shipped:** full CI (Linux, macOS, Windows; both configs); ASan/UBSan daemon job; core-heavy; NodeCore rebuild with lifecycle consumers; MP4 on each platform; a regtest mempool soak (submit, RBF, CPFP, orphan, restart with `mempool.dat`, reorg re-admission). No floor test is run.

## 12. Later projects (listed, not in scope)

- `IMempoolInterface`: implement or delete (E1).
- Move `include/daemon/mempool*.h` and `src/daemon/mempool.cpp` under `include/mempool/`/`src/mempool/` after the legacy `include/mempool/mempool.h` is retired; then drop the path allowlist.
- Make `Params()`/block-index access explicit inputs (wallet spec §12 purity).
- `tx_relay_manager`/`transaction_pool` belong to P2P, not core.
- Delete the dead sources of 6.3.
- Wire `check_no_legacy_mempool_prod` into CI (E2).
- **F1:** make the unguarded chain reads (7.4 table) coherent, e.g. take the read guard on the test-only path; a behaviour change, so separate.

## 13. Left to the builder

- Whether `ct_selection_policy.cpp`/`address_validator.cpp` go into `dinero_mempool` or a small `dinero_policy_codec`; the gates decide (no cycle, no core).
- `IMempoolChainView` return types (X4) and `IMempoolScanSink` exact shape (7.2).
- Whether `daemon/p2p_message.h` include is removed (named edit) or left.
- How MP2 walks the closure (reuse `check_link_closure.py` with a separate rules file, D2).
- The exact MP4 masking list.
- Splitting stages.

**Stop and report**, without working around it, if:
- `mempool.cpp` needs any ChainDB operation beyond 7.1, or any core symbol beyond 6.1;
- any change to M1–M3, `MempoolEntry` or `MempoolTransaction` becomes necessary;
- any consensus file or frozen header would need an edit;
- MP4 differs anywhere;
- the ON-build closure of `dinero_orchard_mempool_body` reaches `dinero_core` or chainstate;
- any gate would need a floor test.

## 14. Owner decisions

**Decided 2026-11-01:**
- **D1.** Inherited consensus violations (bucket A) are reported separately and kept visible; gates fail on mempool-owned violations (bucket B). The library is not described as fully storage-independent until bucket A is gone.
- **D2.** Separate mempool rules and workflow (`scripts/ci/mempool-separation/`, `.github/workflows/mempool-gates.yml`) that reuse the shared tooling once it lands on `dinero-main` (mechanics: X5). **No stacking on #860.**
- **D3.** Prepare the draft PR and its exact run scope; bring the push back for approval with the exact SHA.
- **Prerequisite done:** the wallet plan's header, Global Constraints, Review Focus and Stage 0 were read before the plan was written.

**Open (most important first):**
- **Q0. Does Stage 0 count as implementation?** If yes, Stage 0 waits for X1–X5. If no, Stage 0 may run first; Task 0.8's measurement then applies the X3/X4 *proposals* as assumptions and says so.
- **X1–X5** (9.1): proposals written; to be resolved jointly with the wallet project.
- **L1** (section 8): report the mempool-owned logger edge on its own non-failing row until `dinero_logging` exists — an extension of D1, which covered only consensus-inherited debt.
- **F1** (7.4): the unguarded chain reads are a pre-existing condition; whether to fix them is a separate later decision.
- **Merge order** in the three hot files (section 9) relative to #860.
- **Dependency:** the MP workflow needs #860's scripts on `dinero-main`. If #860's Stage 0 hasn't landed when this Stage 0 is ready, the owner chooses: wait, or run MP1/MP3/MP4 (which need no shared script) and leave MP2/MP5-closure UNMEASURED with that reason.

## Evidence

- Survey at c4940d86c, 2026-11-01: `~/src/dinero-evidence/2026-11-01-mempool-separation/survey/`
  - `mempool_cpp_compilers_off.txt`, `mempool_cpp_compilers_on.txt` (57 targets each, identical);
  - `mempool_o_undefined_off.txt` (`nm -u -C` of `dinerod`'s `mempool.cpp.o`, OFF, AppleClang);
  - `mempool_users_by_target_on.txt` (production users by target);
  - configure logs: `~/src/dinero-sep-builds/mempool-{off,on}.configure.log`.
- **Corrections to the starting facts** in the task prompt:
  - `covenant_policy.cpp` and `policy_engine.cpp` are compiled by **no** target (prompt said root-level targets);
  - `tests/separation/CMakeLists.txt` and `scripts/ci/separation/` exist only on #860's branch, not on `dinero-main`; the 2 refs there are wallet-owned;
  - CMake reference counts split live/commented: `tests/CMakeLists.txt` 100/2, `tests/mining/CMakeLists.txt` 4/2, `src/daemon/CMakeLists.txt` 2/1 (others all live);
  - `check_no_legacy_mempool_prod` is not run by any workflow (E2).
- **`v8.2.0` tag:** `git ls-remote --tags https://github.com/DineroLabs/dinero-v8.git 'v8.2.0*'` returned no tag on 2026-11-01, and no local tag exists; re-checked at stage 0.
- **Overlap with the wallet project** not in its spec: `include/wallet/transaction.h` (wallet-owned path) is included by `include/daemon/mempool.h:22`, `tx_mempool.h:6`, `mempool_events.h:3`, `rbf_policy.cpp:2`, `mempool_policy.cpp:2`, `mempool_persistence.cpp:3`, `ct_selection_policy.h`; it is a thin layer over `primitives/transaction.h` (its own comment: "Wallet uses primitives, never redefines"), and `mempool.o`'s `Transaction::*` symbols resolve to primitives — a header edge into a wallet-owned directory, not a link edge into `dinero_wallet`. MP2 treats it as allowed; moving it is a wallet-project decision. Also: the `mempool_test_stubs.cpp` users (X3) including its own W4 checker; the `dinero_test_mempool_runtime` OBJECT library (32 fixtures, duplicate `tx_relay_manager.cpp`); K2/W2/W6 keyed on path, K3/W3 prefix also matching `MempoolTransaction` (X2); `include/wallet/mempool_interface.h` (`IMempoolInterface`) and `src/wallet/wallet_mempool_adapter.cpp` compiled into `dinero_wallet` against it (E1).
- Wallet spec rev 4 and #860 run summaries (runs 1–4) for the Windows baseline conditions.
