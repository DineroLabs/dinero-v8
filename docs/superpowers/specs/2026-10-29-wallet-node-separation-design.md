# Storage-free consensus kernel and wallet separation — design (rev 4)

**Status:** draft for owner review. No implementation until approved.
**Release:** separate from v8.2.0 and PR #855. Whether any of it fits v8.2.1 is decided **after stage 0 measures the scope** (section 11).
**Baseline:** `dinero-main` **1667f8ed6** (after #854). All file:line references are against that commit, from two read-only re-surveys. Before code work starts the baseline is refreshed again if `dinero-main` has moved, and stage 0 re-checks every cited line.

**Rev 4 changes** (owner review of rev 3, plus the re-survey after #854):
- This milestone is called **storage-free and independently linkable**, not "pure". Full purity becomes a later project, with the global-state inventory in section 12.
- `tx_validation` moves to `const ChainStateView&`, a named change to a frozen header, with its API status recorded and its own gate (6.5).
- The `CONSENSUS_MANIFEST` was found stale and unverified (6.6, E3).
- #854 fixes:
  - gates run with the Orchard backend OFF and ON;
  - the cycle check is scoped to the consensus and wallet closures;
  - rescan takes a nullable view;
  - `WalletMempoolEntry` keeps the Orchard `Historical()` throw;
  - two new UTXOIndex operations;
  - the `selected_history.h` daemon leak;
  - direct `utxo_index.cpp` compiles must be removed;
  - W4 fixtures are generated per configuration.
- `pbkdf2.cpp` and logger facts updated; K4 names corrected and Orchard tests added.

## 1. Target architecture (owner)

The aim is clear ownership and one-way dependencies. **"Pure" is reserved for code that can run from explicit inputs, without a database, network, or global node state.**

- **Consensus kernel:** deterministic validation, scripts, sighash, subsidies, chain rules. It receives height, parameters and coin data as inputs.
- **Chainstate:** stores blocks and UTXOs and adapts them to consensus interfaces.
- **Mempool:** owns unconfirmed transactions and policy. It calls consensus rules; consensus never calls the mempool.
- **P2P:** transports messages. It owns no validation rules.
- **Wallet:** owns keys, accounts and wallet data. Node-side adapters provide chain and mempool observations.
- **Daemon:** assembles the components. `dinero_core` may orchestrate and need not be pure.

The destination is reached in reviewable stages, each enforced by forced-link tests and dependency-graph checks.

This project is the first leg. It produces:
1. a **storage-free, independently linkable** consensus library;
2. a wallet library whose full transitive link has no storage, node, P2P or RPC code.

**Full purity is a later project** (section 12). It replaces process-wide state — selected chain parameters, logger, block-index globals, signature and script caches — with explicit inputs.

## 2. Why now

`dinero_wallet` declares only pure dependencies, but its objects call node code. `dinero_consensus`, which the wallet links, carries RocksDB, SQLite, ChainDB, ChainstateService, wallet and RPC code. Those references resolve only by link-order luck. On GCC 15 the luck runs out: 8 test links fail in CI, 114 in a forced worst case.

PR #856 pinned three jobs to ubuntu-24.04 (`.github/workflows/tests.yml:42`, `:780`, `core-heavy.yml:49`). **The pin is a workaround, and a 24.04 pass says nothing about GCC 15.** The defect stays open until those jobs pass unpinned on GCC 15.

## 3. Goals

1. `dinero_consensus` is **storage-free and independently linkable**:
   - it links on its own (forced, whole-archive) with only declared storage-free libraries;
   - its transitive closure has no RocksDB, SQLite, chainstate, core, wallet, daemon, mempool, P2P or RPC code.
2. The consensus↔wallet and consensus↔pq cycles are gone.
3. `dinero_wallet` links on its own (forced, whole-archive). Its full transitive closure has no RocksDB, chainstate, core, daemon, mempool, P2P or RPC code.
4. Both properties are proven mechanically, in both Orchard-backend configurations, and enforced in CI (section 8).
5. Callers migrate with no external behavior change.
6. The 24.04 pin is removed only after the three jobs pass on GCC 15 without it.

## 4. Hard constraints (owner)

- **Wallet files open unchanged, with no migration.** Wallet storage is SQLite:
  - `<datadir>/wallet_registry.db` and `wallets/wallet_<name>.db`;
  - the UTXOIndex DB `<blockchain>/utxo` (`PRAGMA user_version=2`, ownership migration at `utxo_index.cpp:176-327`);
  - in ON builds, Orchard tables in the wallet DB (`rust/orchard_backend/orchard_wallet_storage.cpp:66,68,139`) and runtime triggers (`runtime_index_delivery.cpp:148,404-409`).

  No schema, path, PRAGMA, migration or trigger change. `open_sqlite`'s PRAGMAs (`src/sqlite_open.cpp:19-40`) move byte-for-byte. Raw `sqlite3_open` sites stay as they are.
- **RPC and FFI unchanged.** RPC methods, fields and errors stay the same. FFI signatures stay the same (`include/nodecore/nodecore_ffi.h`, `wallet-core/ffi/wallet_ffi.h`, JNI exports). Qt and NodeCore behave the same.
- **No consensus logic change.** The one frozen-header change, a parameter type, is named in 6.5 and needs "CONSENSUS CHANGE" approval (`docs/consensus_map.md:590-596`).
- Internal C++ interfaces may change.
- **Any other unavoidable external change is flagged for a separate decision** (9.1), never folded in.

## 5. Non-goals and anti-goals

- No new phone app, light client or FFI surface.
- No behavior change in validation, coin selection, rescan, snapshot recovery, fee bumping, RBF/CPFP, Orchard delivery or sync reporting.
- Dead code is moved or left in place, not deleted; deletion is a follow-up.
- Existing ON-build cycles through `dinero_core` are recorded (section 12), not fixed here.
- **Anti-goal:** a gate that passes because of stubs or link order.
- **Anti-goal:** moving a dependency instead of removing it.

## 6. Leg 1a — storage-free `dinero_consensus`

### 6.1 Classification (53 sources, `src/consensus/CMakeLists.txt:138-192`)

#854 added `utxo_publication.cpp` to both consensus libraries (`:31`, `:152`). Neither it nor any other source #854 touched includes storage, node, wallet or logger headers:
- `chainparams_impl` adds `orchard_profile.h`, which is inline-only;
- `witness_commitment` is a pure refactor;
- `utxo_publication` uses a per-instance mutex.

**43 sources are storage-free.** `src/common/serialization.cpp:3` includes `storage/tip_info.h`, a header-only file over chainwork/uint256; its location is a follow-up.

**10 objects are impure.** Their line numbers are unchanged since rev 3.

| Source | Problem | Live consumers | Disposition |
|---|---|---|---|
| `coins_db.cpp` | RocksDB (`:2-3`, `:31`, `:354`, `:573`) | none compiled (`miner_engine.cpp:356,462,512` is in no CMake file) | **move to `dinero_chainstate`** |
| `coins_view_cache.cpp` | includes `coins_db.h` (RocksDB) | none compiled | **move to `dinero_chainstate`** |
| `tx_validation.cpp` | includes `coins_db.h` (`:2`) for the `CoinsViewCache` type | see 6.5 | **stays; switches to `ChainStateView`** (6.5) |
| `global_utxo_set_impl.cpp` | `daemon/services/chainstate_service.h` (`:11`); ChainstateService/ChainDB calls (`:38,40,58,75`) | none compiled (`fast_sync_service.cpp:321`, `methods_snapshot.cpp:132` are in no CMake file) | **move to `dinero_core`** |
| `src/indexing/utxo_position_index.cpp` | `storage/chain_db.h` (`:12`); ChainDB calls (`:97,175,203,205`) | `chainstate_service.cpp` (core); `methods_utreexo.cpp`; tests `UTXOPositionIndexRebuild` (`tests/CMakeLists.txt:5831-5860`), `CSNIntegration` (`:13335`) | **move to `dinero_chainstate`**. No cycle: chainstate never calls it; core links chainstate (`CMakeLists.txt:1253`) |
| `addr_codec.cpp` | `wallet/address.h` (`:2`); 5 `Address::*` calls | no consensus caller; also compiled into wallet (`src/wallet/CMakeLists.txt:18`) and core (`CMakeLists.txt:995`) | **remove from consensus** |
| `block_index.cpp`, `sigops.cpp` | logger (`block_index.cpp:225` +7; `sigops.cpp:208,216,232`) | — | **stay**; logger declared (6.4) |

**`dinero_pq`** is in the consensus closure: consensus links it PUBLIC (`:207`, `:225`). `src/consensus/pq/CMakeLists.txt` is unchanged by #854.

| Source | Kind | Disposition |
|---|---|---|
| `scheme_registry.cpp`, `ml_dsa_65.cpp`, `ml_dsa_65_keygen.c`, `p2mr_consensus.cpp` | kernel | stay in `dinero_pq`, or fold into `dinero_consensus` if declaring their primitive deps would otherwise make a cycle. The no-cycle and forced-link gates decide. Either way it is a source-list change, not a logic change |
| `src/wallet/pq_derivation.cpp`, `secure_keypair.cpp`, `aead_seed.cpp`, `v7_p2mr_store.cpp` (SQLite), `wallet_key_provider.cpp` | wallet | move to `dinero_wallet` |
| `src/rpc/v7_pq_handlers.cpp` | RPC | move to `dinero_rpc_handlers` |

Effects of the split:
- `dinero_pq` links `sqlite3` PRIVATE (`:58-66`), but the link still propagates because pq is a static library. After the split pq doesn't need it.
- `EncodeP2MRAddress`/`DecodeP2MRAddress` are called only from `v7_pq_handlers.cpp:79,134,244`. Moving that file removes pq's undeclared `dinero_address_encoding` dependency, and with it the consensus → pq → address_encoding → consensus loop.

**Wallet → RPC edge:** `wallet_key_provider.cpp:18` includes `rpc/v7_pq_handlers.h` for `SignP2MRParams`/`SignP2MR` (`:87`, `:104`, `:114`). #854's master-key scrub guard and leaf-hash binding (`:131-134`) depend on that call. The signing function (`v7_pq_handlers.cpp:203`) moves verbatim into a wallet source, and the RPC handlers call it there. RPC behavior is unchanged (W5).

### 6.2 Library interface cleanup

After the moves, `dinero_consensus` drops from its PUBLIC usage requirements:
- `RocksDB::rocksdb` (`:219` Apple, `:232` other) and the RocksDB include dir (`:195-197`);
- `sqlite3` (`:220`, `:233`);
- the `src/daemon` include dir (`:201`). No consensus source needs it.

`jsoncpp_static` stays (`block_undo.cpp:3`).

**Consumers that may rely on those exports** — every target that links `dinero_consensus`:
- `dinero_chainstate` (`src/storage/CMakeLists.txt:35`; it declares RocksDB itself at `:39`) and `dinero_core` (`CMakeLists.txt:1254`);
- pool, network, tools, SwapTower, ffi tests, fuzz;
- ~217 entries in `tests/CMakeLists.txt` and 19 in `tests/consensus`;
- in ON builds, the Orchard libraries that link consensus PUBLIC (`rust/orchard_backend/CMakeLists.txt:204-206`, `:234-235`), and the wallet-scan target that reaches it through block_reader (`:226-227`);
- the Rust bridge.

Stage 0 measures which ones break (section 11). Each fix is the consumer declaring what it uses.

### 6.3 Address cycle and the path rule

- **Address cycle:** removing `addr_codec.cpp` from consensus breaks the consensus↔wallet cycle, since nothing in consensus calls it. `address.cpp`'s five definitions (`:242, 343, 556, 780, 1488`) stay in the wallet.
- **Residual edge:** `dinero_address_encoding` uses `HrpForActiveNetworkRef` (`chainparams_impl.cpp:820`) without declaring it. It is declared only after the pq split; declaring it before the split would close a loop.
- **Path rule — the gates judge code, not paths.** K2/W2 check targets, include directories and cycles; K3/W3 check symbols. Any source under `src/{daemon,rpc,storage,p2p,wallet}` inside a storage-free library must be on an explicit allowlist, with a reason. Today the allowlist holds three entries, all pure codecs:
  - `src/daemon/bech32_decode.cpp` and `bech32_encoder.cpp` (in `dinero_address_encoding`);
  - the header `include/daemon/bech32_decode.h`, used by `src/common/AddressCodec.cpp:7` in the wallet.

### 6.4 Logger and pbkdf2

**`dinero_logging`** (new) contains `src/common/logger.cpp`, which depends only on std and `<filesystem>`.
- It is declared PUBLIC by every library that uses the logger undeclared today: `dinero_crypto` (`encrypted_key.cpp:89-110`), `dinero_consensus`, `dinero_consensus_core` (sigops), `dinero_chainstate`, `dinero_pool`, `dinero_bridge`, `dinero_wallet` and `dinero_core`.
- `logger.cpp` is removed from the wallet's (`src/wallet/CMakeLists.txt:15`) and core's (`CMakeLists.txt:1163`) source lists.
- Test executables that compile `logger.cpp` directly, and the unused `dinero_common` (`src/common/CMakeLists.txt:9`), are left alone.
- Declaring the logger makes the link honest. **It does not make the kernel pure**: the process-wide logger is on the section 12 inventory.

**`pbkdf2.cpp` → `dinero_crypto`.**
- `dinero_crypto`'s `bip39.cpp:61` calls `PBKDF2_HMAC_SHA512`, defined in `src/crypto/pbkdf2.cpp`.
- That file is compiled only into the wallet (`src/wallet/CMakeLists.txt:105`), the wallet reference build and tests.
- #854 rewrote it to use OpenSSL 3 EVP_MAC/OSSL_PARAM. Its includes are now `crypto/pbkdf2.h` plus OpenSSL headers.
- `dinero_crypto` already links OpenSSL::Crypto PUBLIC (`CMakeLists.txt:599`), so the move needs no new link.

### 6.5 `tx_validation` onto `ChainStateView` (frozen-header change, part of this project)

**Today.** `include/consensus/tx_validation.h` (unchanged by #854) declares `validateTransaction` (`:263-268`), `validateInputs` (`:339-344`) and `checkSequenceLocks` (`:361-365`). All three take the concrete `CoinsViewCache&`, forward-declared at `:33`.
- `CoinsViewCache` owns a `CoinsDB*` (`coins_db.h:194`). Moving its declaration into a RocksDB-free header would not make it an interface.
- The existing boundary is `ChainStateView` (`include/consensus/chain_state_view.h:28`): `getCoin`/`hasCoin`/`getHeight`, all `const` pure virtual. `CoinsViewCache` already derives from it (`coins_db.h:194`).
- `tx_validation.cpp` uses only `view.getCoin` (`:97`, `:221`), which is const.

**Change.**
- The three parameters become `const ChainStateView&`.
- The forward declaration becomes `ChainStateView`, or the header includes `chain_state_view.h`, which needs only `outpoint.h`, `utxo_entry.h` and `status.h`.
- `tx_validation.cpp` includes `chain_state_view.h` instead of `coins_db.h`.
- No other line changes in either file.

**API status** (owner's check):
- **Callers:** the only caller of the three functions is `src/mining/miner_engine.cpp:527`, which is in no CMake file. No compiled source, test, tool or fuzz target calls them; the internal calls are at `tx_validation.cpp:475,526`.
- **Not a supported external C++ API:**
  - `cmake/InstallRules.cmake` installs no headers;
  - the NodeCore xcframework ships only `nodecore_ffi.h` (`build_nodecore_xcframework.sh:460`);
  - the ProverKit framework ships only `include/shielded_prover_kit/*.h`;
  - no SDK or documentation offers it externally.

  It is an internal header on the frozen-file list (`docs/consensus_map.md:590-596`).
- **Compatibility:** source-compatible for every caller passing a `CoinsViewCache` (or any `ChainStateView`), since that binds to `const ChainStateView&`. **Not ABI-compatible**: the mangled symbols change. No prebuilt artifact links against them, so this is recorded here and not treated as an external change.
- **Ownership:** the change is explicitly part of this project, under "CONSENSUS CHANGE" approval.

**Gate K6 (frozen-header change):**
- The diff of `tx_validation.h` and `tx_validation.cpp` against the baseline is limited to the three parameter types, the forward declaration or include, and the `.cpp` include line.
- The `CONSENSUS_MANIFEST` entry for `tx_validation.h` (`freeze.h:136`) is updated to the reviewed hash.
- A compile-time test asserts that `CoinsViewCache&` binds to each new signature.
- A grep gate, run on the baseline at stage 0, shows no compiled caller.
- K4 vectors are identical on all four platforms, plus the tx-validation unit tests.

### 6.6 The freeze manifest is stale and unverified (finding)

All three stored hashes in `CONSENSUS_MANIFEST` (`freeze.h:133-141`) are wrong for the current files. For example, `tx_validation.h` stores `b8ef985b…`, but the file hashes to `76b8e063…`. The verifier, `VerifyConsensusIntegrity` (`freeze.cpp:68`), is reached only through `GetConsensusFreezeReport` (`:187`), and **neither has a caller in src, tests or tools.** The manifest protects nothing today.

So this project does not rely on it.
- **K5** records its own baseline SHA-256 for every file under `src/consensus/**` and `include/consensus/**` at 1667f8ed6. It fails on any change outside 6.1, 6.2 and 6.5.
- Correcting the other two stale entries (`limits.h`, `subsidy.h`) and wiring the verifier into a test is **E3**, a separate decision (9.1).
- This project updates only the `tx_validation.h` entry, which it changes.

## 7. Leg 1b — separate the wallet

### 7.1 What the wallet calls (1667f8ed6)

**Sources and links.** `dinero_wallet` sources are at `src/wallet/CMakeLists.txt:14-110`, with hardware-wallet sources at `:133-151`; links are at `:118-130`. #854 added `wallet_transaction_signer.cpp` (`:93`). It also added `dinero_wallet_catalog` (`:9-12`; OpenSSL and sqlite3 only), which the wallet links.

**Orchard backend.** The other new Orchard and runtime wallet files compile only with `DINERO_BUILD_ORCHARD_BACKEND=ON`. That option defaults to OFF (`CMakeLists.txt:1395-1397`) and is ON in `linux-release.yml:146`, `desktop-release.yml:232,531` and `macos-release.yml:163`. Those files build into `rust/orchard_backend` targets that **link the wallet**, not the reverse.

| # | Operation | Call sites | Provider today |
|---|---|---|---|
| C1 | Chain tip | wallet_manager:7557; wallet_sync_status:25; reorg_detector:104; hd_wallet:1070,1214 (`ChainHeightProvider`) | ChainDB `getTip` |
| C2 | Block hash at height | wallet_manager:7638 | ChainDB `getBlockHashByHeight` |
| C3 | Read block | wallet_manager:7640 → `storage::ReadArchivalBlock` (`include/storage/archival_block_reader.h:70,110`) | ChainDB + optional BlockStorage |
| W1–W3 | UTXO lookup, list unspent, register/clear watch scripts | GetUTXO 6310; Clear 2878; Register 7160, 7315, 10141, 10333, 10478, 10523, **10673 (new)**; hd_wallet:1018-1050, 1073, 1218, 1490; transaction_builder:95 | UTXOIndex |
| W4 | Merge-register watch scripts | wallet_manager:10108 (**new**) | `UTXOIndex::MergeRegisteredAddresses` |
| W5 | Atomic index rewrite during snapshot recovery | wallet_manager:7875-7881 (**new**): `ApplyAtomically`, `GetUnspentUTXOs`, `DeleteUTXO`, `GetUTXO`, `AddUTXO` | UTXOIndex |
| S1 | Open SQLite with standard PRAGMAs | wallet_manager:425, 1811, 2069; descriptor_store:20 | `open_sqlite` |
| M1 | Outpoint spent in mempool | wallet_manager:6317, **6342 (new, snapshot-candidate filter)** | `Mempool::isOutputSpentInMempool` |
| M2 | Mempool entry | rbf_cpfp_detector:85,141,182,261; fee_bump_engine:190,260 | `Mempool::getMempoolEntry`, **now a `MempoolTransaction`** (`mempool.h:82`) |
| M3 | Mempool stats | slow_reason_analyzer:210 | `Mempool::getStats` |
| D1–D3 | Sync phase/header progress; peer quality; tx inclusion | wallet_sync_status:44,54; slow_reason_analyzer:141; wallet_fee_bump_hooks:26-27,106,172,214,244,311 | core / p2p / mining |
| P1 | Address → scriptPubKey | descriptor_psbt:253,391; recipient_descriptor:18 | `BuildScriptPubKeyFromAddress` (`CMakeLists.txt:1166`, pure) |
| T1 | Snapshot wallet data types | `include/wallet/selected_history.h:3` → `daemon/services/snapshot_wallet_source.h` (types only; the wallet never calls `ReadSnapshotWalletSource`) | daemon header (**new leak**) |

No `dinero_wallet` source calls ChainstateService, PeerManager or Orchard services.

**Compile-time leaks to remove:**
- `storage/chain_db.h:11-16` (RocksDB headers), included by wallet_manager (`:93`) and the diagnostics;
- `daemon/mempool.h:4-7,24-25`;
- node types in public headers: `include/wallet/wallet_manager.h:45-58,174-175,604,796-797,1150-1151`, `wallet_worker.h:18,169`, `selected_history.h:3`, `wallet_sync_status.h:3`, `wallet_fee_bump_hooks.h:7`.

### 7.2 Shape

```
dinero_logging    (new)  src/common/logger.cpp
dinero_sqlite     (new)  src/sqlite_open.cpp, unchanged; linked by wallet and chainstate
dinero_crypto            + pbkdf2.cpp; declares dinero_logging
dinero_pq                kernel sources only (6.1)
dinero_consensus         storage-free after leg 1a
dinero_wallet            links consensus, pq, logging, sqlite, wallet_catalog, crypto, bip32,
                         tx_primitives, address_encoding, shielded, argon2, OpenSSL, sqlite3
                         + utxo_index.cpp (from chainstate), address_script_builder.cpp (from core),
                           the five pq wallet sources, p2mr_signing.cpp (moved SignP2MR)
                         defines IWalletChainView, IWalletMempoolView
dinero_core              + wallet_sync_status.cpp, slow_reason_analyzer.cpp, wallet_fee_bump_hooks.cpp,
                           global_utxo_set_impl.cpp
include/storage/chain_db_wallet_view.h   header-only ChainDbWalletView
include/daemon/mempool_wallet_view.h     header-only MempoolWalletView
```

**UTXOIndex** (`src/wallet/utxo_index.cpp`).
- It is SQLite only: `open_sqlite` at `:104`. Its includes are utxo_index.h, sqlite_open.h, hd_wallet.h, addr_codec.h and logger.h. The RocksDB comment at `src/storage/CMakeLists.txt:19` is wrong.
- It **moves into the wallet**, so W1–W5 become internal calls.
- The node still creates and owns the instances (`chainstate_service.cpp:3385`; temporaries at `:11062`, `:13597`, `:13723`; `daemon_app.cpp:1898`; `setUTXOIndex` at `wallet_service.cpp:195`).
- **Prerequisite:** `utxo_index.cpp` is also compiled directly into ~18 test entries (e.g. `tests/CMakeLists.txt:905`, `:4438`) and two Orchard tests (`rust/orchard_backend/CMakeLists.txt:663,675`) that also link the wallet. Those direct compiles are removed in the same stage, or they become duplicate definitions.

**Interfaces** (`include/wallet/node_interfaces.h`). Only what 7.1 requires:

```cpp
struct WalletChainTip { int32_t height; uint256 hash; };
class IWalletChainView {                    // C1-C3
public:
    virtual ~IWalletChainView() = default;
    virtual std::optional<WalletChainTip> GetTip() const = 0;
    virtual std::optional<uint256> GetBlockHashAtHeight(int32_t height) const = 0;
    virtual std::optional<CBlock> ReadBlock(const uint256& hash) const = 0;
};
class IWalletMempoolView {                  // M1-M3
public:
    virtual ~IWalletMempoolView() = default;
    virtual bool IsOutputSpent(const OutPoint&) const = 0;
    virtual std::optional<WalletMempoolEntry> GetEntry(const uint256& txid) const = 0;
    virtual WalletMempoolStats GetStats() const = 0;   // total_size, tx_count, avg_fee_rate
};
```

Field types are copied from the current Mempool and ChainDB types.

**M2 after #854.**
- `rbf_cpfp_detector.cpp:100` reads `entry.tx.Historical().vin`, and `Historical()` throws `std::logic_error` for Orchard entries (`mempool_transaction.h:75-78`).
- `WalletMempoolEntry` carries `fee`, `tx_size` and `depends`, and inputs that exist only for historical transactions.
- Its `vin()` accessor throws the **same exception type and message at the same point of use**, so the detector behaves identically.
- The W5 transcript includes an Orchard mempool entry in ON builds.

**Rescan takes a nullable chain view:** `rescanBlockchain(start, gap, const IWalletChainView* view = nullptr)`. A per-call reference cannot express the paths that exist after #854:

| Caller | View | Effect |
|---|---|---|
| `wallet_service.cpp:668-672`, `methods_wallet_context.cpp:6593-6597`, `nodecore_ffi.cpp:1348-1353` | with BlockStorage, as today | unchanged |
| `wallet_worker.cpp:217` | without BlockStorage (keeps the ChainDB-only read path) | unchanged |
| ChainstateService prepared-history path (`chainstate_service.cpp:14538`, `:14667`), via the private `RescanBlockchainImpl(…, const SelectedWalletHistory*, uint64_t)` (`wallet_manager.h:1150-1151`) | null | unchanged: the `if (!chain_db && !prepared)` guard (`:7507`) becomes `if (!view && !prepared)` |

**Other wallet-side moves:**
- **Adapters are header-only.** Core, rpc_handlers, nodecore_ffi and tests compile them in. Every includer already links chainstate or compiles `mempool.cpp`, so there's no new library and no core cycle.
- **Diagnostics (D1–D3) move to `dinero_core` unchanged.** `reorg_detector`, `rbf_cpfp_detector` and `fee_bump_engine` stay in the wallet and use the interfaces.
- **`SyncPhase`** moves unchanged to `include/common/sync_phase.h`, which `block_download_scheduler.h` includes. The `WalletSyncStatus` struct stays in the wallet.
- **T1:** `SnapshotWalletSourceData`/`SnapshotWalletCoin` move unchanged to `include/wallet/snapshot_wallet_types.h`, which `daemon/services/snapshot_wallet_source.h` includes. `selected_history.h` includes the new header.

**Friend access stays in leg 1.** `ChainstateService`, `RuntimeOrdinaryDelivery`, `RuntimeWalletRecovery`, `OrchardAccountDelivery` and `RuntimeIndexDelivery` are friends of `WalletManager`/`UTXOIndex` (`wallet_manager.h:381,1142,1157-1159`; `utxo_index.h`).
- These classes live outside the wallet and link it, so they don't enter the wallet's closure.
- Removing node code's reach into wallet internals is a later leg (section 12).
- A WalletManager refactor can break code that only compiles in ON builds, so every gate runs in both configurations.

## 8. Gates (written first, observed on the baseline)

Each gate is added in stage 0 as a non-required job so its baseline result is recorded. It becomes required in stage 7. **Every gate runs with `DINERO_BUILD_ORCHARD_BACKEND=OFF` (default) and `=ON` (release configuration).**

| Gate | What | Baseline expectation |
|---|---|---|
| **K1 consensus forced link** | Executable linking `--whole-archive libdinero_consensus.a` plus only its declared deps (`--no-undefined` semantics). `main` calls `Params()` and verifies one script vector and one sighash vector | RED: 16 undefined node symbols |
| **K2 consensus closure** | Walk `dinero_consensus` usage requirements recursively, including PRIVATE links of static libraries. Fail on: RocksDB; sqlite3; chainstate, core, wallet or rpc_handlers; any target compiling `src/daemon/mempool.cpp`; RocksDB or `src/daemon` include dirs; a cycle **anywhere in the consensus closure**; a non-allowlisted source under node directories (6.3) | RED: RocksDB, sqlite3 (direct and via pq), daemon include, pq's wallet and RPC sources, consensus↔pq cycle |
| **K3 consensus symbols** | `nm -C` on K1's output finds no `rocksdb::`, `sqlite3_`, `ChainDB`, `ChainstateService`, `Address::` or `Mempool` | RED |
| **K4 consensus vectors identical** | Before and after, `ctest --no-tests=error`; the log must show each listed test ran, and every printed digest must match (test list below the table) | GREEN (reference run) |
| **K5 consensus source freeze** | Baseline SHA-256 of every file under `src/consensus/**` and `include/consensus/**` at 1667f8ed6. Fails on any change outside 6.1/6.2/6.5 (CMake lists, the pq split, the `tx_validation` lines) | GREEN |
| **K6 frozen-header change** | as in 6.5 | introduced in stage 2 |
| **W1 wallet forced link** | `--whole-archive libdinero_wallet.a` plus only its declared deps; no `wallet_test_deps`, no `mempool_test_stubs.cpp`. `main` constructs a `WalletManager` on a temp dir and opens it | RED: the 10 known `wallet_manager.o` undefined refs plus the D1–D3 core/p2p/mining symbols |
| **W2 wallet closure** | Same checks as K2 on `dinero_wallet`, and also fails on full `dinero_core`, `dinero_rpc_handlers` and P2P. Allows third-party crypto (`lightning_core_static` = libwally/secp256k1-zkp/blake3, via `dinero_zk`). The cycle check covers the wallet closure | RED: RocksDB via consensus; pq's RPC source |
| **W3 wallet symbols** | As K3 on W1's output, plus `BlockStorage`, `HeaderSyncManager`, `PeerManager`, `TxInclusionAnalyzer` | RED |
| **W4 wallet-file compatibility** | **One fixture per configuration**, because the schema differs in ON builds. Each holds a registry, a wallet DB (with Orchard tables in ON), an HD wallet, a descriptor store, and a UTXOIndex DB at `user_version=2`, generated by the 1667f8ed6 build. The split build opens each read-write and reports identical addresses, balance, UTXOs and `user_version`. Each file's `sqlite_master` dump, including triggers, is identical before and after | GREEN |
| **W5 external interface freeze** | Empty diff of `nodecore_ffi.h`, `wallet_ffi.h` and the JNI export list; unchanged exported dynamic symbols of `nodecore_ffi` and `dinero_wallet_ffi`. Regtest transcript identical field for field: getbalance, getnewaddress, listunspent (including the mempool-spent filter), sendtoaddress, rescan, snapshot-recovery rescan, fee bump/RBF, sync status, 3 error cases, and in ON builds an Orchard mempool entry seen by RBF detection | GREEN |
| **W6 standalone consumer** | `dinero_wallet_ffi` and the Rust bridge's static set link with no node libraries. **Record-only, never required (E1)** | measured in stage 0 |

K4's test list:
- **`tests/CMakeLists.txt`:**
  - `BIP341SighashVectors` (`:6212`), `BIP119CTVVectors` (`:6169`), `CovenantActivation` (`:6233`);
  - `ScriptJSONTests` (`:8345`), `ScriptInterpreter` (`:6101`);
  - `DAAGoldenVectors` (`:2046`), `UtreexoLeafHashGolden` (`:13543`), `MerkleGolden` (`:11957`);
  - `test_header_hash_vectors` (`:11820`), `HeaderPoWVerification` (`:12017`);
  - `TaprootConsensus` (`:6081`), `TaprootScriptPathConsensus` (`:6190`);
  - `RIPEMD160*Vectors` (`:366,865,880`), `HashWrapperVectors` (`:897`);
  - `StateCommitmentExternalVectors` (`:2417`);
  - `CompactRegtestFixedVectors` (`:16301`), `CompactRegtestVectorOracle` (`:16312`).
- **`tests/consensus/CMakeLists.txt`:**
  - `SerializationVectors` (`:699`), `ConsensusCoreSt*` (`:1344`), `StatelessVerification` (`:1229`);
  - `SubsidySchedule` (`:1222`), `ConsensusInvariants` (`:1267`), `FormalInvariants` (`:1155`);
  - `Consensus_DeterminismOracles_DD1_DD5_R5_5f` (`:639`).
- **ON builds also** (`rust/orchard_backend/CMakeLists.txt`): `OrchardMemoryPublication` (`:633`, covers the new `utxo_publication.cpp`), `OrchardHeader` (`:606`), `OrchardStateRoot` (`:645`), `OrchardStateCommitment` (`:657`), `OrchardCompactHeader*`.
- **Workflows:** `compact-regtest-vectors.yml`, plus the independent-vectors and saved-proofs jobs.

**Cross-platform.** K1–K3, K6, W1–W3 and K4 run on Linux GCC 15 (ubuntu-latest), Linux GCC 13 (24.04), macOS Clang (arm64) and Windows MSVC.
- Windows uses a CI-only workflow modeled on `windows-openssl-release-smoke.yml`: no signing, no uploads.
- MSVC watch item: the `ParamsImpl` LNK2005 history (root `CMakeLists.txt:520-534`). `tx_validation.o` references `detail::ParamsImpl`.
- Forced-link flags: `-force_load` on Apple, `/WHOLEARCHIVE` on MSVC.

**Cycle check scope.** K2/W2 cycle checks cover only the consensus and wallet closures. **On ON builds the full graph already has cycles through `dinero_core`**: `dinero_orchard_chainstate_write`, `_reindex` and `_header` PRIVATE-link core, while core PUBLIC-links them (`rust/orchard_backend/CMakeLists.txt:249,300,307-309,426,430`). Those are recorded in section 12, not fixed or gated here.

## 9. Affected consumers

| Consumer | Change | Behavior |
|---|---|---|
| `dinero_chainstate` | gains `coins_db`, `coins_view_cache`, `utxo_position_index`; links `dinero_sqlite` and `dinero_logging`; loses `utxo_index.cpp` | unchanged |
| `dinero_core` | gains `global_utxo_set_impl` and the three diagnostics; loses `address_script_builder.cpp` and `logger.cpp` | unchanged |
| `dinero_crypto` | gains `pbkdf2.cpp`; declares logging | unchanged |
| `dinero_pq` and its consumers | keeps the kernel; its wallet and RPC sources move. Consumers get them from the wallet or rpc_handlers (counted in stage 0) | unchanged |
| `dinero_rpc_handlers` | gains `v7_pq_handlers.cpp`; calls `SignP2MR` from the wallet | unchanged; W5 |
| Orchard backend targets (ON) | `dinero_orchard_index_delivery` and others keep linking the wallet. Their friend access and direct `utxo_index.cpp` compiles are adjusted. Orchard libs that link consensus declare what they relied on it exporting | unchanged; W4/W5 in ON |
| `dinerod`, `nodecore_ffi`, NodeCore xcframework (`build_nodecore_xcframework.sh`) | link set gains `dinero_logging` and `dinero_sqlite`; the script bundles them | unchanged; W5 |
| `WalletService`, RPC, NodeCore C ABI, `wallet_worker`, ChainstateService | build views from ChainDB plus optional BlockStorage, or pass null (7.2). **Lines matched by `qt/tests/test_shielded_rpc_status.cmake:7-39` are not edited** | unchanged; W5 |
| Standalone wallet FFI; Rust bridge | no change (E1) | unchanged |
| Qt | none (separate process) | unchanged |
| Tools, pool, network, SwapTower, fuzz, ffi tests | declare their own RocksDB, sqlite and daemon includes where they relied on consensus (measured) | unchanged |
| Tests (~88 link the wallet; ~236 link consensus) | pure-wallet tests drop `wallet_test_deps` and `mempool_test_stubs.cpp`; tests needing a mempool use a fake `IWalletMempoolView`; integration tests link core and include the header-only adapters; direct `utxo_index.cpp` compiles are removed | unchanged |

### 9.1 Flagged for separate decision (not folded in)

- **E1.** The split may make the standalone wallet FFI and Rust bridge link (W6). That is a fix, but it changes what those builds produce.
- **E2.** `getBalance(const void* mempool_ptr)` ignores its argument (`wallet_manager.cpp:5609`), as does `getScriptPubKeyBalance` (`:5838`). Left as is.
- **E3.** The `CONSENSUS_MANIFEST` entries for `limits.h` and `subsidy.h` are stale, and nothing verifies the manifest (6.6). Correcting them and wiring `VerifyConsensusIntegrity` into a test is a separate decision.

## 10. Stages

Each stage is its own PR onto `dinero-main`, and must be green on existing CI plus the named gates, in **both** Orchard configurations, before the next one starts.
- Each gate is observed failing before the change that turns it green.
- Logs are grepped to show the tests ran.
- Touched sources are rebuilt from clean.

| Stage | Content | Exit |
|---|---|---|
| **0 — refresh, gates, measurement** | Refresh the baseline to the then-current `dinero-main` and re-check every cited line. Add K1–K5 and W1–W6 (non-required). Build the W4 fixtures (OFF and ON), the W5 transcripts and the K4/K5 reference data. **Measurement:** on a throwaway branch, apply all of leg 1a (6.1–6.5), build every target on every platform in both configurations, and record what breaks and why | K1–K3 and W1–W3 RED as expected; K4, K5, W4, W5 GREEN; W6 and the breakage list recorded → **owner decides v8.2.1 fit** |
| 1 | `dinero_logging`, declared by all eight users; `pbkdf2.cpp` → crypto | full CI; K4 identical |
| 2 | Consensus moves (6.1); pq split; `SignP2MR` → wallet; `tx_validation` → `ChainStateView` (6.5, "CONSENSUS CHANGE" approval); then `dinero_address_encoding` declares consensus | K1, K3, K6 GREEN; K4, K5 identical on all four platforms; no cycles in the consensus closure |
| 3 | Drop consensus exports (6.2); consumers declare their own deps | K2 GREEN; full CI on all platforms |
| 4 | `dinero_sqlite`; `address_script_builder.cpp` and `utxo_index.cpp` → wallet; remove direct `utxo_index.cpp` compiles | W4 GREEN (both fixtures); full CI |
| 5 | Interfaces, header-only adapters, nullable-view rescan, `WalletMempoolEntry`; wallet sources and headers rid of node includes (including T1); caller migration | W1–W3 GREEN; W4, W5 GREEN |
| 6 | Diagnostics → core; `sync_phase.h`; test link cleanup | every gate except W6 GREEN on all four platforms, both configurations |
| 7 | Gates become required. Unpinned trial copies of the three pinned jobs run on ubuntu-latest (non-required) | trial jobs GREEN on GCC 15 |
| 8 | Only after the stage-7 trials pass: remove the pin and comment at `tests.yml:42`, `:780`, `core-heavy.yml:49`; drop the trial copies | the three jobs GREEN on ubuntu-latest |

## 11. Scope measurement and the v8.2.1 decision

**Known now:**
- **Consensus:** 10 impure objects.
  - 4 are moves with live users: utxo_position_index, the `tx_validation` switch, the addr_codec removal and the logger.
  - 3 have no compiled caller: coins_db, coins_view_cache and global_utxo_set_impl.
- **pq:** 6 non-kernel sources, plus the `SignP2MR` move.
- **Crypto:** the pbkdf2 crypto → wallet edge.
- **Wallet:**
  - 14 operations (C1–C3, W1–W5, S1, M1–M3, P1, T1);
  - 3 diagnostics files;
  - 5 rescan paths;
  - ~20 direct `utxo_index.cpp` compiles.
- **Tests:** ~324 test references.

**Unknown until stage 0:**
- how many consumers rely on consensus exporting RocksDB, sqlite3 and `src/daemon`;
- how much ON-build Orchard code is affected;
- whether the standalone wallet FFI links today.

Stage 0 reports counts per platform and configuration, and an estimate per stage. The v8.2.1 decision is made on that report.

**Requalification for anything shipped:**
- full Linux, macOS and Windows CI, in both configurations;
- the ASan/UBSan daemon job;
- compact regtest qualification and vectors;
- independent vectors and saved proofs;
- core-heavy;
- a NodeCore rebuild with its lifecycle and snapshot consumers;
- W4/W5 on each platform;
- a regtest wallet soak: send, receive, rescan, snapshot recovery, fee bump, reorg.

The consensus library's composition changes and one frozen header changes, so K4 and K6 are mandatory on every platform.

## 12. Later projects and follow-ups (listed, not in scope)

**Full purity of the consensus kernel** (owner's definition). Inventory of process-wide or external state in `dinero_consensus` at 1667f8ed6:
- **Selected chain parameters** (`chainparams_impl.cpp`):
  - mutable `g_mainnet` (`:61`), `g_testnet` (`:302`), `g_regtest` (`:408`);
  - `g_active` (`:589`), `g_paramsSelected` (`:590`);
  - an unlocked lazy write of `g_regtest.genesis` in `ParamsImpl` (`:598-608`);
  - `SelectParams` (`:742`), `GetActiveChain` (`:776`), `IsChainSelected` (`:802`), `MutableParams` with `const_cast` (`:813-817`), `HrpForActiveNetworkRef` (`:820`).

  `Params()` call sites: 15 lines in consensus, ~752 in src and include, ~302 in tests. `SelectParams` and `MutableParams` have ~536 test sites between them.
- **Logger:** `Logger`/`g_logger` in `block_index.cpp` and `sigops.cpp`.
- **Block-index globals:** `g_block_index_mutex` (`block_index.cpp:29`) and `g_candidates` (`:32`), extern in `block_index.h:211,217`, used by chainstate_service, block_acceptor, orchard_reindex and methods_vault. This is chain state living in consensus.
- **Global caches:** `g_signature_cache` (`signature_cache.cpp:11`, reset at `:156`) and `g_script_cache` (`script_cache.cpp:11`, reset at `:180`).
- **Environment and host input:** `getenv("DINERO_FOREST_SERIALIZE_LEGACY_V2")` (`utreexo_accumulator.cpp:2522`); host CPU load via Mach (`cpu_budget_monitor.cpp:315-327`).

**Toward the target architecture:**
- Mempool as a library (`src/daemon/mempool.cpp` belongs to no library); `dinero_rpc_handlers` then declares it.
- Remove node code's friend access to wallet internals: `ChainstateService`, `Runtime*Delivery`, `RuntimeWalletRecovery`, `OrchardAccountDelivery`, `RuntimeIndexDelivery`.
- Fix the ON-build cycles through `dinero_core` (`rust/orchard_backend/CMakeLists.txt:249,300,307-309,426,430`).
- `include/consensus/orchard_*.h` headers that include `storage/*` or daemon headers (e.g. `orchard_catalog_validation.h:4`, `validation_queue.h:36`).
- Reconcile `dinero_consensus_core` with `dinero_consensus`. consensus_core is not self-contained: it needs `VerifyTaproot`, `SignatureHash*`, `Params` and `GetActiveChain`.
- Limit P2P to transport; remove the `dinero_core` → `dinero_rpc_handlers` edge via `vault_runtime`.
- `include/wallet/runtime_replay_spool.h` is used by node code (`orchard_snapshot_store.cpp:117`).

**Cleanup:**
- Delete dead, uncompiled code:
  - `src/mining/miner_engine.*`, `fast_sync_service.cpp`, `methods_snapshot.cpp`, `utreexo_proof_generator.cpp`;
  - `src/daemon/rpc/WalletHandlers.cpp`, `src/core/rpc/wallet_{security,query}_rpc_handlers.cpp`;
  - `src/blockchain/real_blockchain_integration.cpp`, `src/multi_account/multi_account_manager.cpp`, `src/mobile/mobile_wallet{,_simple}.cpp`, `src/wallet/address_minimal.cpp`;
  - then `global_utxo_set_impl` and `coins_db`/`coins_view_cache`, if still unused.
- Duplicate objects: `addr_codec`/`addr_types`, `block_index`, `src/wallet/psbt.cpp`.
- Misplaced but pure: `storage/tip_info.h`; the bech32 codecs under `src/daemon`.
- `FFI_WALLET_ONLY` is never defined; `BUILD_FFI_ONLY` is not exercised in CI.
- `IMempoolInterface` has no implementer.
- `CrossLanguageVectors` is commented out (`tests/consensus/CMakeLists.txt:789`).
- Other 24.04 pins.

## 13. Left to the builder

- Exact interface field types.
- Whether chainstate links the wallet after UTXOIndex moves.
- pq kernel placement (6.1).
- How K2/W2 walk the closure.
- The exact W5 transcripts.
- Splitting stages.

**Stop and report**, without working around it, if:
- any consensus source beyond 6.1 needs storage or node code;
- any frozen header needs a change beyond 6.5;
- K4 digests differ;
- an external change beyond E1–E3 appears;
- an ON-build Orchard consumer needs wallet internals beyond the friend list in 7.2.

## Evidence

- Re-surveys of 1667f8ed6 (this revision).
- `~/src/dinero-evidence/2026-10-28-wallet-node-separation/`: forced-link probe, baseline RED, `undeclared.json` from the earlier base.
- `~/src/dinero-evidence/2026-10-28-rocksdb-gcc15/`.
- PR #856, merged as 3aca093c0: the pin this project removes.
- PR #854, merged as 1667f8ed6: the current baseline.
