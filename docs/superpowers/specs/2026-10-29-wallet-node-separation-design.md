# Wallet / node separation — design

**Status:** draft for owner approval. No code until approved.
**Target:** v8.2.1 at the earliest, *if* the requalification cost below is acceptable. Not part of v8.2.0 or of PR #855.
**Baseline:** `dinero-main` 3aca093c0 (after #856). All file:line references are against that commit.

## 1. Why

`dinero_wallet` declares that it depends only on pure libraries, but ten of its objects call node code: ChainDB, BlockStorage, UTXOIndex, `open_sqlite`, `dinero::Mempool` (`src/daemon/mempool.cpp`, which is in no library), and a few `dinero_core` symbols. Today those references resolve only by link-order luck: GNU ld scans each archive once, and `wallet_manager.cpp.o` is pulled in or not depending on which archive member first satisfies a weak libstdc++ instantiation that RocksDB also references. On GCC 15 (ubuntu-latest) that luck runs out: 8 test links fail in CI, 114 in a forced worst case.

PR #856 pinned the three affected CI jobs to ubuntu-24.04 (`.github/workflows/tests.yml:42`, `:780`, `core-heavy.yml:49`). **The pin is a workaround, not a fix.** A 522/522 pass on 24.04 says nothing about GCC 15; the defect stays open until the wallet links on GCC 15 without the pin.

## 2. Goals

1. `dinero_wallet` builds and passes a **forced-link test** (every object in the archive linked, whole-archive) with **no ChainDB, UTXOIndex-in-chainstate, BlockStorage, RocksDB, daemon mempool, or `dinero_core` code** anywhere in its *transitive* link.
2. That property is **proven mechanically** and kept by CI (section 6), not asserted.
3. The node supplies what the wallet needs through **interfaces the wallet defines**, implemented by **node-side adapters**.
4. Existing callers — daemon/RPC, NodeCore, the standalone wallet FFI, Qt — migrate with no external behavior change (section 3).
5. The ubuntu-24.04 pin on the three jobs is removed **only after** those jobs pass on GCC 15 without it.

## 3. Hard constraints (owner)

- **Wallet files open unchanged, no migration.** Wallet storage is SQLite only: `<datadir>/wallet_registry.db` (`wallet_manager.cpp:396`), `<datadir>/wallets/wallet_<name>.db` (`:1695`, `:1923`), schema via `PRAGMA user_version` (`:2827`, `:2840`). No schema, file name, path, or PRAGMA changes. `open_sqlite`'s PRAGMAs (WAL, `synchronous=NORMAL`, `foreign_keys=ON`, busy 5000, mmap 64 MB — `src/sqlite_open.cpp:19-40`) move byte-for-byte. The raw `sqlite3_open` sites (wallet_manager:2088, hd_wallet:308/684/744, wallet_utxo_tracker:61, asset_registry:69) are left exactly as they are, including their lack of PRAGMAs.
- **RPC methods, fields, and error behavior unchanged.**
- **FFI signatures unchanged:** `include/nodecore/nodecore_ffi.h`, `wallet-core/ffi/wallet_ffi.h`, and the 7 Android JNI exports.
- **Qt and NodeCore behave the same.**
- **Internal C++ interfaces may change** (constructors, setters, method parameters, headers under `include/wallet/`).
- **Any unavoidable external change is flagged for a separate decision**, not folded in. Section 9 lists the candidates found so far.

## 4. Non-goals and anti-goals

- No new phone app, no light client, no new FFI surface.
- No behavior change in coin selection, rescan, fee bumping, RBF/CPFP, sync reporting.
- Not fixing the other architecture tangles (section 10); they are listed only.
- **Anti-goal:** a "separation" that passes because a test links stubs or because link order happens to work. If the gate can pass with `wallet_manager.cpp.o` absent, it is not a gate.
- **Anti-goal:** moving the dependency instead of removing it (e.g. the wallet linking a new library that itself drags in RocksDB).

## 5. What the wallet actually calls (derived from the code)

Two read-only surveys of 3aca093c0 traced every node reference from `dinero_wallet` sources and reconciled it against the symbol evidence (`~/src/dinero-evidence/2026-10-28-wallet-node-separation/undeclared.json`, `link-red-base.log`). Ten objects, not nine: `wallet_fee_bump_hooks.cpp` was missing from the earlier list.

| # | Operation | Call sites | Today's provider |
|---|---|---|---|
| C1 | Chain tip (height, hash) | wallet_manager:5682, wallet_sync_status:25, reorg_detector:104; hd_wallet:1070,1214 via the existing `ChainHeightProvider` | ChainDB `getTip` |
| C2 | Block hash at height | wallet_manager:5831 | ChainDB `getBlockHashByHeight` |
| C3 | Read full block by hash (archival read) | wallet_manager:5839 → `storage::ReadArchivalBlock` (`include/storage/archival_block_reader.h:76,81,104`: `getHeaderMetadata`, `BlockStorage::readBlock`, `ChainDB::getBlock`) | ChainDB + BlockStorage |
| W1 | Watch-index UTXO lookup `(txid,vout)` → exists, is_confidential | wallet_manager:4395 | UTXOIndex `GetUTXO` |
| W2 | List unspent from watch index | hd_wallet:1073,1218; transaction_builder:95 | UTXOIndex `GetUnspentUTXOs` |
| W3 | Register watch script / clear watch set | wallet_manager:2547, 5262, 5503, 8189, 8297, 8489, 8634, 8679; hd_wallet:1018-1050, 1490 | UTXOIndex `RegisterAddress`, `ClearRegisteredAddresses` |
| S1 | Open SQLite DB with standard PRAGMAs | wallet_manager:401, 1712, 1941; descriptor_store:20 | `open_sqlite` (in dinero_chainstate) |
| M1 | Is outpoint spent in mempool | wallet_manager:4454 (listUnspent filter) | Mempool `isOutputSpentInMempool` |
| M2 | Mempool entry by txid → fee, size, vin, depends | rbf_cpfp_detector:85,141,182,261; fee_bump_engine:190,260 | Mempool `getMempoolEntry` |
| M3 | Mempool stats (size, count, avg fee rate) | slow_reason_analyzer:210 | Mempool `getStats` |
| D1 | Sync phase, header-sync progress | wallet_sync_status:44,54 | BlockDownloadScheduler (inline), `HeaderSyncManager::GetStatus` (core) |
| D2 | Peer quality stats | slow_reason_analyzer:141 | `PeerManager::GetQualityStats` (core) |
| D3 | Tx inclusion analysis | wallet_fee_bump_hooks:26-27, 106/172/214/244/311 | `TxInclusionAnalyzer` (`src/mining`, core) |
| P1 | Address → scriptPubKey | descriptor_psbt:253,391; recipient_descriptor:18 | `BuildScriptPubKeyFromAddress` (`src/common/address_script_builder.cpp`, core; pure code) |

Not in the set: transaction submit. The wallet makes no submit call today (NodeCore's `nodecore_broadcast_tx` goes to the mempool directly, `nodecore_ffi.cpp:1187-1232`). `IMempoolInterface` (`include/wallet/mempool_interface.h:137`) exists with no production implementer; it is left alone.

**Compile-time leaks** that must go too, or the wallet only compiles because `dinero_consensus` exports RocksDB's include path:
- `storage/chain_db.h:8-13` includes `<rocksdb/*>`; included by wallet_manager, wallet_sync_status, reorg_detector, slow_reason_analyzer.
- `daemon/mempool.h:20-21` pulls in `mempool/coins_view_mempool.h` and `mining/ct_selection_policy.h`.
- Public wallet headers name node types: `include/wallet/wallet_manager.h:42-46, 139, 399, 580-581`; `wallet_sync_status.h:3` (`p2p/block_download_scheduler.h`); `wallet_fee_bump_hooks.h:7`.

## 6. The transitive problem: `dinero_consensus`

Removing the ten objects' node calls is **not enough**. `dinero_wallet` PUBLIC-links `dinero_consensus` (`src/wallet/CMakeLists.txt:112-123`), and `dinero_consensus`:
- PUBLIC-links `RocksDB::rocksdb` (`src/consensus/CMakeLists.txt:217`, `:230`) for `coins_db.cpp`, and exports RocksDB's include dir (`:193-195`) and `src/daemon` (`:199`);
- contains objects that reference `ChainstateService::GetChainDB` (dinero_core) and ChainDB methods without declaring either (`global_utxo_set_impl.cpp:11,38,58`; `src/indexing/utxo_position_index.cpp:12,175,205`);
- has a consensus→wallet cycle (`addr_codec.cpp` calls `Address::*`).

So the wallet's transitive link contains RocksDB today regardless of what the wallet's own objects do.

**Decision (proposed):** the wallet stops linking `dinero_consensus` and links `dinero_consensus_core` instead. `dinero_consensus_core` (`src/consensus/CMakeLists.txt:29-76`) has no RocksDB, but lacks three things the wallet uses: `chainparams_impl.cpp` (`Params()`, e.g. hd_wallet:879), `script_interpreter.cpp`, `script_sighash.cpp` (taproot_tx_signer, swap/htlc, covenant_profile, transaction_signer). Their direct includes are consensus/primitives/crypto headers only (checked); they move from `dinero_consensus` to `dinero_consensus_core`, and `dinero_consensus` keeps getting them by linking `dinero_consensus_core` as it does now. The builder confirms the full transitive include and symbol closure with the gate in section 7; if any of the three turns out to drag storage in, that is a stop-and-report, not a workaround.

`addr_codec.cpp` / `addr_types.cpp` are compiled into three targets (wallet :13-14, consensus :181-182, core :959-962). The wallet keeps its own copy; the cycle in `dinero_consensus` is a follow-up (section 10).

## 7. Proof of separation (the gates)

These are written first, observed RED on the baseline, and become required CI checks at the end.

- **G1 — forced standalone link.** A test executable links `-Wl,--whole-archive libdinero_wallet.a -Wl,--no-whole-archive` plus *only* the wallet's declared transitive libraries, with `-Wl,--no-undefined` semantics (an executable link; no stubs, no `wallet_test_deps`, no `mempool_test_stubs.cpp`). It must link and run a trivial `main` that constructs a `WalletManager` on a temp dir and opens it. Evolves `test_wallet_mempool_link.cpp` from the evidence folder, which fails deterministically on the baseline with 10 undefined references. Whole-archive is what removes link-order luck: every object is present, so any undeclared dependency fails every time.
- **G2 — transitive library closure.** A CMake script walks `dinero_wallet`'s `INTERFACE_LINK_LIBRARIES` recursively and fails if the closure contains `RocksDB::*`, `rocksdb`, `dinero_chainstate`, `dinero_core`, `dinero_consensus` (the full one), `dinero_rpc_handlers`, or any target compiling `src/daemon/mempool.cpp`. Also fails if any include directory in the closure is RocksDB's or `src/daemon`.
- **G3 — symbol check.** On the G1 binary: `nm -C` must show no `rocksdb::`, `dinero::ChainDB`, `dinero::BlockStorage`, `dinero::Mempool`, `ChainstateService`, or `HeaderSyncManager` symbols. Catches a dependency that sneaks in through a library G2 thinks is clean.
- **G4 — wallet file compatibility.** A fixture wallet directory (`wallet_registry.db` + one `wallets/wallet_<name>.db`, HD wallet, descriptor store) created by the *pre-split* build and checked in. The post-split build opens it read-write and must report the same addresses, balance, UTXOs, and `user_version`, and must not change the file's schema (`sqlite_master` dump identical before and after open).
- **G5 — external interface freeze.** Header diff of `nodecore_ffi.h`, `wallet_ffi.h`, and the JNI export list is empty against the baseline; the exported dynamic symbol list of `nodecore_ffi` and `dinero_wallet_ffi` is a superset-equal match. RPC: the existing wallet RPC tests stay green, plus a recorded regtest transcript (getbalance, getnewaddress, listunspent incl. mempool-spent filtering, sendtoaddress, rescan, fee-bump/RBF, sync status, plus 3 error cases) diffed field-for-field before vs after.
- **G6 — standalone consumer.** `dinero_wallet_ffi` and the Rust bridge's static link set (`mobile-tauri/src-tauri/build.rs:29-38`: `dinero_wallet_ffi`, `dinero_wallet`, `dinero_crypto`, `dinero_consensus` → becomes `dinero_consensus_core`) link with no node libraries. The survey *infers* this link is broken today; stage 0 confirms or refutes it by building it.

G1–G3 run on GCC 15 (ubuntu-latest), GCC 13 (24.04), Clang (macOS), and MSVC (Windows).

## 8. Shape of the change

### 8.1 Libraries

```
dinero_wallet            pure; defines the interfaces; links consensus_core (+3 files), crypto,
                         bip32, tx_primitives, address_encoding, shielded, argon2, OpenSSL, sqlite3,
                         dinero_sqlite
dinero_sqlite   (new)    src/sqlite_open.cpp only; linked by dinero_wallet and dinero_chainstate
dinero_wallet_node (new) node-side adapters + node diagnostics moved out of the wallet;
                         links dinero_wallet, dinero_chainstate, dinero_core
```

- `open_sqlite` (S1) moves into `dinero_sqlite` unchanged. Both the wallet and chainstate (`utxo_index.cpp:96` uses it) link it, so there is no second copy.
- `BuildScriptPubKeyFromAddress` (P1) is pure; `src/common/address_script_builder.cpp` moves from `dinero_core` into `dinero_wallet` (core already links the wallet, so core callers are unaffected).
- **UTXOIndex (W1–W3).** `src/wallet/utxo_index.cpp` is SQLite, not RocksDB (the comment at `src/storage/CMakeLists.txt:17` is wrong), and includes only wallet/address/logger headers. It moves from `dinero_chainstate` into `dinero_wallet`; W1–W3 then become internal wallet calls, not interfaces. The node still creates and owns the instance (`chainstate_service.cpp:2595`, `daemon_app.cpp:1724`) and hands it in with `setUTXOIndex` as today. If `dinero_chainstate`'s own objects (not `ChainstateService` in core) use UTXOIndex, chainstate links `dinero_wallet` for it; the builder checks which.

### 8.2 Interfaces (wallet-defined, `include/wallet/node_interfaces.h`)

Only what section 5 requires:

```cpp
struct WalletChainTip { int32_t height; uint256 hash; };

class IWalletChainView {            // C1, C2, C3
public:
    virtual ~IWalletChainView() = default;
    virtual std::optional<WalletChainTip> GetTip() const = 0;
    virtual std::optional<uint256> GetBlockHashAtHeight(int32_t height) const = 0;
    virtual std::optional<CBlock> ReadBlock(const uint256& hash) const = 0;
};

struct WalletMempoolEntry { Transaction tx; CAmount fee; size_t tx_size; std::set<uint256> depends; };
struct WalletMempoolStats { size_t total_size; size_t tx_count; double avg_fee_rate; };

class IWalletMempoolView {          // M1, M2, M3
public:
    virtual ~IWalletMempoolView() = default;
    virtual bool IsOutputSpent(const OutPoint& outpoint) const = 0;
    virtual std::optional<WalletMempoolEntry> GetEntry(const uint256& txid) const = 0;
    virtual WalletMempoolStats GetStats() const = 0;
};
```

Exact field types follow what the current callers read (the builder copies them from `Mempool`'s entry/stats types; the shapes above are the fields used, not new semantics). `ChainHeightProvider` already used by hd_wallet stays and is implemented by the chain-view adapter, so hd_wallet needs no change beyond wiring. Null view = today's null pointer behavior (e.g. `listUnspentUTXOs` with no mempool skips the M1 filter, exactly as `nodecore_list_unspent_json` does now, `nodecore_ffi.cpp:1150-1161`).

### 8.3 Node diagnostics move out instead of being abstracted

D1–D3 are node telemetry, used only by RPC handlers and tests. Wrapping them in wallet interfaces would put node concepts in the wallet's API. Instead `wallet_sync_status.cpp`, `slow_reason_analyzer.cpp`, and `wallet_fee_bump_hooks.cpp` (and their headers) move from `dinero_wallet` to `dinero_wallet_node`, keeping names and behavior. `reorg_detector.cpp` needs only C1, so it stays in the wallet on `IWalletChainView`. `rbf_cpfp_detector.cpp` and `fee_bump_engine.cpp` stay in the wallet on `IWalletMempoolView` (they are wallet logic). *My call; the alternative is three more interfaces.*

### 8.4 Adapters (`dinero_wallet_node`)

- `ChainDbWalletView : IWalletChainView, ChainHeightProvider` wraps `ChainDB*` + `BlockStorage*`; `ReadBlock` calls `storage::ReadArchivalBlock` exactly as wallet_manager:5839 does now.
- `MempoolWalletView : IWalletMempoolView` wraps `const Mempool*`. Because `src/daemon/mempool.cpp` is in no library, this adapter's .cpp is compiled into the same targets that compile `mempool.cpp` (`dinerod`, `nodecore_ffi`, and the 6 tests that compile it) rather than into `dinero_wallet_node`. Putting the mempool in a library is a follow-up (section 10).

## 9. Caller migration

| Caller | Today | After | External effect |
|---|---|---|---|
| `WalletService` (`src/daemon/services/wallet_service.cpp`) — sole daemon/NodeCore owner | passes ChainDB/BlockStorage per call (:335, :502), UTXOIndex via setter (:114) | builds `ChainDbWalletView` once from `chainstate_->GetChainDB()` + `block_storage_`; `wm->setChainView(&view)`; `rescanBlockchain(start, gap)` uses it. UTXOIndex setter unchanged | none |
| RPC (`dinero_rpc_handlers`) | `methods_wallet_context.cpp:1890-1908` casts MempoolService to `Mempool*` for `listUnspentUTXOs`; `:6678-6682` passes chain_db/block_storage to rescan; `wallet_sync_rpc_handlers.cpp:36,44`, `fee_bump_rpc_handlers.cpp:43,51,63-75` pass raw node pointers | pass `MempoolWalletView` / rely on the wallet's chain view. **Do not edit `methods_wallet_context.cpp` lines matched by `qt/tests/test_shielded_rpc_status.cmake:7-39`** (`result["account_index"] = 0`, `GetMasterFingerprintHex()`) | none; G5 transcript |
| NodeCore (`src/nodecore/nodecore_ffi.cpp`) — uses `DaemonApp`, never constructs a wallet | `nodecore_rescan` passes ChainDB/BlockStorage (:1118-1140); `nodecore_list_unspent_json` passes no mempool (:1150-1161) | rescan uses the wallet's chain view; list-unspent unchanged (still no mempool view, so output is identical). C ABI untouched | none; G5 |
| Standalone wallet FFI (`wallet-core/ffi`) | `WalletManager(datadir)` with no node objects (`wallet_ffi.cpp:219`); `TransactionBuilder(wm->getUTXOIndex())` always null | unchanged code; now actually links standalone (G6) | possibly "starts linking" — see below |
| Rust/Tauri bridge (`mobile-tauri/src-tauri/build.rs:29-38`) | links `dinero_consensus` | links `dinero_consensus_core` | build-script change only |
| Qt (`qt/`) | separate process over JSON-RPC/WS; links no wallet code | no change | none |
| Tests (~88 executables link the wallet) | node deps satisfied by `wallet_test_deps` (`tests/CMakeLists.txt:799-810`, ~23 uses), `tests/stubs/mempool_test_stubs.cpp` (36 uses), real `mempool.cpp` (6), direct `wallet_manager.cpp` compiles (6) | tests of pure wallet logic drop those and link `dinero_wallet` alone; tests that need a mempool use a fake `IWalletMempoolView`; node-integration tests link `dinero_wallet_node`. `mempool_test_stubs.cpp` shrinks to whatever non-wallet tests still need | none |

**Candidate external changes, flagged for separate decision (none folded in):**
- **E1.** If G6 confirms the standalone wallet FFI / Rust bridge does not link today, the split makes it link. That is a fix, but it changes what that build produces; decide separately whether to ship or advertise it.
- **E2.** `getBalance(const void* mempool_ptr)` ignores its argument (wallet_manager.cpp:3933). Left as is; changing it would change RPC results.
- None other found. If the builder finds one, stop and add it here.

## 10. Stages

Each stage is its own PR onto `dinero-main`, green on the existing CI before the next starts. G-tests are added in stage 0 as **non-required** jobs so their RED is visible, and turn required at stage 5.

| Stage | Content | Exit check |
|---|---|---|
| 0 | Add G1–G6 (G4 fixture generated from the baseline build). Record their baseline RED in the PR. | G1/G2/G3 RED with the expected undefined refs / RocksDB in closure; G4/G5 GREEN on baseline; G6 result recorded |
| 1 | `dinero_sqlite`; `address_script_builder.cpp` → wallet; `utxo_index.cpp` → wallet | full CI green; G4 green |
| 2 | Wallet links `dinero_consensus_core` (+ chainparams_impl, script_interpreter, script_sighash moved there) | G2 no longer reports RocksDB via consensus; full CI green |
| 3 | `IWalletChainView` / `IWalletMempoolView`, adapters, wallet sources and public headers rid of node includes; caller migration (section 9) | G1/G2/G3 GREEN; G4/G5 GREEN |
| 4 | Diagnostics move to `dinero_wallet_node`; test link cleanup | G1–G6 GREEN on GCC 15, GCC 13, Clang, MSVC |
| 5 | Make G1–G6 required. Add **unpinned trial copies** of the three pinned jobs on ubuntu-latest (non-required). | trial jobs GREEN on GCC 15 |
| 6 | Only after stage 5's trial jobs pass: remove the pin and its comment at `tests.yml:42`, `:780`, `core-heavy.yml:49`; drop the trial copies | the three jobs GREEN on ubuntu-latest |

Every stage follows the repo's verification rules: each gate observed failing before the change that turns it green; logs grepped to show the tests ran; touched sources rebuilt from clean.

## 11. Requalification cost (deciding factor for v8.2.1)

The split changes the link of `dinerod`, `nodecore_ffi`, and the NodeCore xcframework (`build_nodecore_xcframework.sh:254-374` bundles `libdinero_wallet.a`). No consensus or wallet *logic* changes, but anything that ships must be requalified:
- full Linux/macOS/Windows CI, ASan/UBSan daemon job, compact regtest qualification, core-heavy;
- NodeCore rebuild and its lifecycle/snapshot consumers;
- G4 wallet-file compatibility and the G5 RPC transcript on each platform;
- a regtest wallet soak (send, receive, rescan, fee bump, reorg) on the release candidate.

**Recommendation:** decide the v8.2.1 target after stage 2. Stages 0–2 are small and low-risk and tell us whether `dinero_consensus_core` can carry the wallet; stage 3 is the bulk of the work.

## 12. Follow-ups (listed, not in scope)

- `dinero_consensus` is impure: RocksDB via `coins_db.cpp`; undeclared ChainDB/ChainstateService use in `global_utxo_set_impl.cpp` and `src/indexing/utxo_position_index.cpp`; consensus→wallet cycle via `addr_codec.cpp`.
- `src/daemon/mempool.cpp` belongs to no library and is compiled into ~21 targets.
- `dinero_core` → `dinero_rpc_handlers` via `vault_runtime`.
- `src/common/logger.cpp` compiled into both `dinero_wallet` and `dinero_core`; the wallet's copy currently satisfies undeclared logger use in `dinero_crypto`, `dinero_consensus`, `dinero_chainstate`, `dinero_bridge`. **Not moved here**: the standalone link works with the wallet's own copy, so moving it does not serve this project's goal. Fix with a small `dinero_logging` library later.
- `addr_codec.cpp`/`addr_types.cpp` in three source lists; `src/wallet/psbt.cpp` in wallet and core.
- Uncompiled dead sources: `src/daemon/rpc/WalletHandlers.cpp`, `src/core/rpc/wallet_security_rpc_handlers.cpp`, `wallet_query_rpc_handlers.cpp`, `src/blockchain/real_blockchain_integration.cpp`, `src/multi_account/multi_account_manager.cpp`, `src/mobile/mobile_wallet{,_simple}.cpp`.
- `FFI_WALLET_ONLY` is referenced (`wallet_manager.h:88-94`) but defined by no CMake file; `BUILD_FFI_ONLY` is never exercised by CI.
- `IMempoolInterface` has no production implementer.
- Other workflows pin ubuntu-24.04 without the wallet-link comment (desktop-release, linux-release, compact-regtest, compact-package-boundaries, shielded-readiness, …); out of scope.
- Comment at `src/storage/CMakeLists.txt:17` wrongly says UTXOIndex uses RocksDB (fixed incidentally in stage 1 when the file moves).

## 13. Left to the builder

- Exact field types in the interface structs (copied from the current `Mempool` / ChainDB types they replace).
- Whether `dinero_chainstate` itself needs to link `dinero_wallet` after UTXOIndex moves (section 8.1).
- How G2 walks the closure (CMake script vs. generated link-line inspection), as long as it fails on the listed targets.
- The exact regtest transcript in G5, within the methods listed.
- Splitting any stage into smaller PRs.

## Evidence

- `~/src/dinero-evidence/2026-10-28-wallet-node-separation/` — forced-link probe, baseline RED log, undeclared-symbol analysis.
- `~/src/dinero-evidence/2026-10-28-rocksdb-gcc15/` — GCC 15 container and CI link diagnosis.
- PR #856 (merged 3aca093c0) — the pin this project removes.
