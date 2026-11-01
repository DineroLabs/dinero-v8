# Mempool as a library (`dinero_mempool`) — design (rev 1)

**Status:** draft for owner review. No implementation until approved.
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
5. is declared by every consumer (`dinero_core`, `dinero_rpc_handlers`, `dinero_wallet` until the wallet's own seam lands, `dinerod`, `nodecore_ffi`, tests) instead of being compiled into each one.

## 2. Why now

- **`src/daemon/mempool.cpp` belongs to no library.** It is compiled directly into **57 targets** (identical OFF and ON): `dinerod`, `nodecore_ffi`, `swap_e2e_fixture_daemon`, the OBJECT library `dinero_test_mempool_runtime` (attached to 32 fixtures by `tests/CMakeLists.txt:17204-17309`), and 53 test executables (list: evidence `survey/mempool_cpp_compilers_off.txt`).
- **Two-way dependency with `dinero_core`.** `dinero_core` (20 sources, e.g. `mempool_service.cpp`, `chainstate_service.cpp`, `p2p_service.cpp`, `tx_relay_manager.cpp`, `block_assembler.cpp`) calls `Mempool::` but does not contain it; `mempool.cpp` in turn needs `dinero_core` objects (section 6.1). Both resolve only because the final executable compiles `mempool.cpp` in. `dinero_rpc_handlers` (16 sources) and `dinero_wallet` (5 sources) have the same undeclared edge.
- **Stubs instead of a library.** `tests/stubs/mempool_test_stubs.cpp` replaces six `Mempool` methods with no-ops for 19 configured targets (43 live references in `tests/CMakeLists.txt`, 4 in `rust/orchard_backend/CMakeLists.txt:533,544,663,675`). The wallet project's W1/W6 gates forbid it; it can't go away while there is no real library to link.
- **The wallet project needs it.** Its W2/W6 closures forbid `src/daemon/mempool.cpp`; its §7 mempool seam (M1–M3) is header-only over `Mempool`. A real `dinero_mempool` with a storage-free closure is what lets that seam be linked honestly.

## 3. Goals

1. `dinero_mempool` exists and links on its own (forced, whole-archive) with only declared dependencies.
2. Its transitive closure contains no `dinero_core`, `dinero_chainstate`, `dinero_wallet`, `dinero_rpc_handlers`, P2P, gRPC, RocksDB or sqlite3 code, and no `src/daemon` include directory (except the mempool's own headers, allowlisted, section 6.4).
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

`dinerod`'s `mempool.cpp.o` was compiled (OFF, macOS, AppleClang) and `nm -u -C` taken (evidence `survey/mempool_o_undefined_off.txt`). Every non-std undefined symbol, by defining library:

| Group | Symbols | Defined in (library) | Disposition |
|---|---|---|---|
| Consensus kernel | `ParamsImpl`, `ValidateSpend`, `PrecomputedTransactionData`, `CoinbaseMaturity::isCoinbaseMature`, `FindBlockIndex`, `CBlockIndex::GetMedianTimePast`, `Transaction::*`, `uint256::*` | `dinero_consensus` (also `dinero_consensus_core`, `dinero_tx_primitives`) | **allowed**; declared PUBLIC |
| pq | `consensus::ComputeVWU` | `src/consensus/pq/p2mr_consensus.cpp` → `dinero_pq` | **allowed**; declared |
| Shielded | `shielded::BuildShieldedValidationContext`, `DeserializeShieldedBundle`, `ValidateShieldedBundle` | `dinero_shielded` | **allowed**; declared |
| Chainstate | `ChainDB::getTip` (12 call sites), `getCoinWithConfidentialFallback` (5), `getTxLocation` (2), `forEachUTXO` (2) | `dinero_chainstate` (RocksDB) | **remove**: replaced by `IMempoolChainView` (section 7) |
| Mempool-owned, in core today | `CoinsViewMemPool::*`, `FeeEstimator::*` (`src/mempool/fee_estimator.cpp`), `MempoolPersistence::{load,save}`, `policy::RBFPolicy::*` (`src/policy/rbf_policy.cpp`), `DecodeRelayTransaction` (`src/daemon/relay_transaction_reader.cpp`) | `dinero_core` (`CMakeLists.txt:1043-1049`, `:991-995`, `:1208`) | **move into `dinero_mempool`** (source-list change) |
| Mining (wrong direction) | `mining::CTSelectionPolicy::{CTSelectionPolicy,GetWeightInfo}` (`ct_selection_policy.cpp`, includes only `chainparams.h`); `mining::DecodeAddress`, `mining::BuildScriptPubKey` (`address_validator.cpp`, includes only `bech32_decode.h`, `base58.hpp`) | `dinero_core` (`CMakeLists.txt:1120`, `:1127`) | **move into `dinero_mempool`** as pure policy/codec objects, or a small lower library; builder decides (section 13). Both are storage-free. `mempool.h:25` and `rbf_policy.h:4` include `mining/ct_selection_policy.h` |
| Privacy | `din::sp::ScannerManager::{ScannerManager,~ScannerManager,scanMempoolTransaction}`, `acquireEmptyRegistry` (`mempool.cpp:468`, `:793-794`, `:1104-1106`) | `src/core/privacy/silent_scanner_manager.cpp` → `dinero_core` (the copy at `src/privacy/` compiles nowhere) | **inject**: section 6.2 |

Includes of `mempool.cpp` with no remaining symbol use: `daemon/p2p_message.h` (`:11`) shows no `P2PMessage`/`p2p::` reference. Removing it is a named edit, verified by compile, or it is left (section 13).

**Logger:** `mempool.cpp` logs through the injected `ILogger` (`MPLOG_*`, `setLogger`); `fee_estimator.cpp`, `mempool_persistence.cpp`, `tx_orphan_pool.cpp`, `mempool_policy.cpp` and `rbf_policy.cpp` include `common/logger.h`. The library declares the wallet project's `dinero_logging` once it exists (wallet Stage 1); until then that edge is in bucket B (section 8).

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
- The three in-file ChainDB adapters (`ChainDBUTXOView` `:108`, `ChainDBStateView` `:187`, the `MempoolUTXOView` member `:281`) switch from `ChainDB*` to `const IMempoolChainView*`. **This is the only logic-adjacent edit in `mempool.cpp`**: a type and include swap, each call one-to-one.
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

`ScannerManager::acquireEmptyRegistry()` (`include/privacy/silent_scanner_manager.h:32-35`) returns a `unique_ptr<EmptyRegistry>` that **holds `scanners_mutex_` for its lifetime**, and `mempool.cpp:793` keeps it alive through the rest of Orchard admission. The interface therefore returns an RAII guard, not a bool; the core implementation wraps `EmptyRegistry` in it, so lock scope and ordering are unchanged. Owned by `Mempool` as `std::unique_ptr` (same lifetime as today). The `ScannerManager`-backed implementation lives in core. Where `Mempool` is constructed without a sink, behaviour must equal today's "manager always present" default, so the constructor keeps creating the default sink when the caller passes none — **or** every construction site passes one; stage 0 counts which test sites rely on the default.

### 7.3 Unchanged

`isOutputSpentInMempool`, `getMempoolEntry`, `getStats`, `MempoolEntry`, `MempoolTransaction`, `UTXOView`, `MempoolAccess`, `ITxIngress`, all observer and guard types.

## 8. Gates (written first, observed on the baseline)

Each gate is added at stage 0 as a non-required job; its baseline result is recorded. Every gate runs with `DINERO_BUILD_ORCHARD_BACKEND=OFF` and `=ON`, on Linux GCC 15 (ubuntu-latest), Linux GCC 13 (ubuntu-24.04), macOS (macos-15, Xcode 16.4) and Windows MSVC.

**Two buckets.** At c4940d86c `dinero_consensus` itself still carries RocksDB, sqlite3, the `src/daemon` include dir and pq's wallet/RPC sources, and `dinero_logging` doesn't exist (wallet K1/K2 RED; #860). `dinero_mempool` must link consensus, so its closure inherits those violations. MP1–MP3 therefore report:
- **Bucket A — inherited:** reachable only through `dinero_consensus`, `dinero_pq` or the undeclared logger. Owned by wallet K2; reported, not failing, until K2 is green.
- **Bucket B — mempool-owned:** everything else. Must be zero for the gate to pass.

Stage exits (section 10) require bucket B = 0; full green (A and B) follows wallet Stages 1–3. **Owner decision D1.**

| Gate | What | Baseline expectation |
|---|---|---|
| **MP1 forced link** | Executable linking `dinero_mempool` whole-archive (`--whole-archive` / `-force_load` / `/WHOLEARCHIVE`) plus only its declared deps, `--no-undefined` semantics. `main` constructs a `Mempool` on a fake `IMempoolChainView` and calls `size()` | RED: target doesn't exist. After creation, before the interfaces: undefined `ChainDB::*`, `ScannerManager::*`, plus bucket A |
| **MP2 closure** | Walk `dinero_mempool` usage requirements recursively (incl. PRIVATE of static libs). **Allowed:** `dinero_consensus`, `dinero_consensus_core`, `dinero_tx_primitives`, `dinero_pq` (kernel), `dinero_shielded`, `dinero_crypto`, `dinero_logging`, and in ON `dinero_orchard_mempool_body` and its closure. **Forbidden:** `dinero_core`, `dinero_chainstate`, `dinero_wallet`, `dinero_rpc_handlers`, P2P targets, gRPC, RocksDB, sqlite3, `src/daemon` include dirs, any target compiling `tests/stubs/mempool_test_stubs.cpp`, cycles in the closure, non-allowlisted node-path sources (6.4). **Reverse check:** no target in the `dinero_consensus` closure links `dinero_mempool` | RED (target missing) |
| **MP3 symbols** | `nm -C` on MP1's output: no `rocksdb::`, `sqlite3_`, `ChainDB`, `ChainstateService`, `din::sp::`, `PeerManager`, `P2PService`, `RPCServer`, `WalletManager`, `grpc::` | RED |
| **MP4 behaviour freeze** | (a) **RPC transcript** on regtest, field for field (timestamps and other run-varying fields masked by an explicit, reviewed list): `getmempoolinfo`; `getrawmempool` (plain and verbose); `getmempoolentry`; `getmempoolancestors`/`descendants`; `testmempoolaccept` (accept, reject, duplicate); `sendrawtransaction`; an RBF replacement (accepted and rejected); a CPFP child; an orphan arriving before its parent; `estimatesmartfee`/`estimatefee`; three error cases; in ON builds an Orchard mempool entry (accepted, and the scanner-UNAVAILABLE path if reachable over RPC, else by a unit test). (b) **Persistence:** a `mempool.dat` fixture per configuration generated by the c4940d86c build (ON contains an Orchard entry, because load decodes `AvailableFamilies`, `mempool.cpp:4242`); the split build loads it, reports identical entries, saves it, and the saved file is **byte-identical** to the fixture | GREEN (reference run) |
| **MP5 standalone consumer** | `sep_mempool_standalone`, linked only through `target_link_libraries(... dinero_mempool)` (no `dinero_core`, no stubs, no hand-listed archives). On a fake chain view it must **run**: submit a valid tx (accepted), an invalid one (rejected with the frozen reason), save, clear, load, compare. Its resolved closure must pass MP2's rules | RED (target missing) |

**MP4 and the never-run floor.** `MempoolPolicyOwner`, `MempoolTestAcceptReadOnly`, `BlockRelayPoolOwner`, `CsnCpfpDisconnect`, `AddressBalanceMempoolOverlay`, `UtreexoMempoolCanonicalSeparation` and other mempool-adjacent tests are on the floor (`scripts/ci/separation/never_run_floor.txt` on #860's branch, 230 names). MP4 uses its own transcript and fixture checks and runs **no** floor test; the gate script refuses any name on the floor list.

**Known baseline conditions** (from #860 runs; `~/src/dinero-evidence/2026-10-29-separation-stage0/ci-pr860-run{1..4}/SUMMARY.md`):
- Windows: `dinerod.exe` exited rc 1 at startup under the transcript harness (run 1) → MP4(a) on Windows may be **UNMEASURED** at baseline; recorded, not hidden.
- Windows ON: `LNK2005` duplicate `isCoinbaseMature` (`src/consensus` + `src/core/consensus`) (run 2) → ON links on MSVC may fail at baseline for reasons outside this project.
- `nodecore_ffi` is not an MSVC target (owner, 2026-10-30).

## 9. Affected consumers

| Consumer | Change | Behaviour |
|---|---|---|
| `dinero_core` | loses the 8 mempool-owned sources + `ct_selection_policy`, `address_validator` (if they move); links `dinero_mempool` PUBLIC; `MempoolService` builds `ChainDbMempoolView` and the scanner sink | unchanged; MP4 |
| `dinero_rpc_handlers` | declares `dinero_mempool` | unchanged |
| `dinero_wallet` | declares `dinero_mempool` **only until** the wallet project's M1–M3 seam lands (cross-project, X1) | unchanged |
| `dinerod` (both `add_executable` branches, `src/daemon/CMakeLists.txt:58`, `:133`), `nodecore_ffi` (`src/nodecore/CMakeLists.txt:50`), `swap_e2e_fixture_daemon` | drop the direct `mempool.cpp`; link the library (via core). The NodeCore xcframework script bundles `libdinero_mempool.a` | unchanged; W5 (wallet) and MP4 |
| Orchard backend (ON) | `dinero_mempool` links `dinero_orchard_mempool_body`; 4 stub users at `rust/orchard_backend/CMakeLists.txt:533,544,663,675` re-decided with X3 | unchanged |
| Tests | 53 direct compiles and `dinero_test_mempool_runtime` (32 fixtures) replaced by linking `dinero_mempool`; 46 test files that construct `Mempool(ChainDB*,…)` pass `ChainDbMempoolView` (mechanical). Duplicate `tx_relay_manager.cpp` compile goes with the OBJECT library | unchanged |
| Tests using `mempool_test_stubs.cpp` (19 configured; 43 + 4 refs) | **X3** — they define `Mempool::` methods that the real archive also defines; any target that links `dinero_mempool` (via core) and the stubs gets duplicate definitions (LNK2005 on MSVC, link-order luck elsewhere). Each must either drop the stubs and link the library, or not link core | unchanged |

**Hot shared files** (both projects edit; list for merge ordering, section 10):
- root `CMakeLists.txt`: blocks `:991-995` (policy sources), `:1043-1049` (mempool support), `:1102`/`:1120`/`:1127`/`:1208` (single lines in `dinero_core`'s list), one new `add_library(dinero_mempool …)` block placed **after** `dinero_core`'s source sets and before `add_library(dinero_core` (`:1063`), and one `target_link_libraries(dinero_core PUBLIC dinero_mempool)` line next to core's existing link block (`:1253`ff).
- `tests/CMakeLists.txt`: the 50 direct `mempool.cpp` entries (52 refs, 2 commented), the `dinero_attach_real_mempool_fixture_runtime` function (`:17204-17309`), and the stub entries only as X3 decides.
- `rust/orchard_backend/CMakeLists.txt`: `:180-184` (body link, relay-reader property), `:533,544,663,675` (stubs, per X3).
- Also `tests/mining/CMakeLists.txt:147,195,374,461`, `src/daemon/CMakeLists.txt:58,133`, `src/nodecore/CMakeLists.txt:50`.

### 9.1 Flagged for decision (not folded in)

**Cross-project (both projects; owner decides):**
- **X1. Wallet's mempool edge.** `dinero_wallet` uses M1–M3 today. Until the wallet's `IWalletMempoolView` seam lands, linking `dinero_mempool` from the wallet would put the mempool **inside** W2/W6's closure, which they forbid. Proposal: the wallet does **not** declare `dinero_mempool`; its 5 sources keep resolving through the final link as today, and the wallet project's Stage 5 removes the edge. This project adds no wallet edit.
- **X2. Wallet gate rules must learn the new target.** K2/W2/W6 forbid the *path* `src/daemon/mempool.cpp`; after this project they should also forbid the *target* `dinero_mempool`. K3/W3 forbid symbol prefix `dinero::Mempool`, which also matches `dinero::MempoolTransaction`/`MempoolEntry` — relevant if `WalletMempoolEntry` wraps those types. The rules file is wallet-owned; proposed text only.
- **X3. `mempool_test_stubs.cpp`.** Used by 19+4 targets and by the wallet project's `tests/separation/CMakeLists.txt:77-80` (W4 checker). Once core links the real library, stubs plus core is an ODR violation. Options: (a) each stub user drops core; (b) each drops the stub and links `dinero_mempool`; (c) stubs stay only where neither core nor `dinero_mempool` is linked. Needs a joint decision because W4/W6 depend on it.
- **X4. `Coin` type location** for `IMempoolChainView` (7.1). `Coin` is only in `storage/chain_db.h:37`, so a storage-free copy or a `consensus::UTXOEntry`-based return is needed. The wallet's planned `IWalletChainView` may hit the same type; consensus headers are wallet-owned, so any new header goes under `include/daemon/` or `include/mempool/` and both projects should agree on one.

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

## 14. Open owner decisions (most important first)

- **D1. Bucket split** (section 8): accept that MP1–MP3 pass on bucket B (mempool-owned) while bucket A (inherited through `dinero_consensus`/`dinero_pq`/logger) waits on wallet Stages 1–3.
- **D2. Gate tooling:** (a) a separate rules file `scripts/ci/mempool-separation/mempool_closure_rules.json` plus workflow `.github/workflows/mempool-gates.yml`, calling #860's scripts read-only once they are on `dinero-main`; or (b) stack on #860 after its Stage 0 lands. Recommendation: (a) — no edits in wallet-owned files, and the scripts are reused unmodified.
- **D3. CI trigger:** a new workflow on a non-default branch runs only on `pull_request`, so measuring stage 0 on Linux/Windows needs a **draft PR** (push approval per SHA).
- **X1–X4** (9.1): wallet's mempool edge, wallet gate rules learning `dinero_mempool`, `mempool_test_stubs.cpp`, the `Coin` type.
- **Merge order** in the three hot files (section 9) relative to #860.

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
- **Overlap with the wallet project** not in its spec: the `mempool_test_stubs.cpp` users (X3) including its own W4 checker; the `dinero_test_mempool_runtime` OBJECT library (32 fixtures, duplicate `tx_relay_manager.cpp`); K2/W2/W6 keyed on path, K3/W3 prefix also matching `MempoolTransaction` (X2); `include/wallet/mempool_interface.h` (`IMempoolInterface`) and `src/wallet/wallet_mempool_adapter.cpp` compiled into `dinero_wallet` against it (E1).
- Wallet spec rev 4 and #860 run summaries (runs 1–4) for the Windows baseline conditions.
