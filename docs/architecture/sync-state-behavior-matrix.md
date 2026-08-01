# Sync-state behavior matrix (#439)

Design note for the canonical sync snapshot. Written **before** implementation so the intended
behavior is reviewable independently of the code.

> **Implementation update (issue #441):** the raw `GetBestHeader()`, `GetHeader()`,
> `GetHeaderAtHeight()`, and pointer-based `FindForkPoint()` APIs described below
> have since been removed. Production and tests now use value copies, identity
> checks, or compute-under-lock ancestry operations. The table and hazard
> analysis are retained as the design history that motivated that change; they
> do not describe the current accessor surface.

## Why this exists

`dinero::HeaderSyncManager` (`include/consensus/header_sync_manager.h`) documents itself as the
*"CANONICAL IBD DEFINITION (FINAL)"* — IBD is true iff best header != active chain tip. It is
**never instantiated in production**: `g_header_sync_manager` is never assigned, and
`BlockRelayManager::SetHeaderSyncManager()` is never called. Every consumer null-guards it and
falls through to a different mechanism. See #439.

Consequences at the time this design was written:

| Consumer | Falls back to | Effect |
|---|---|---|
| `nodecore_is_synced()` (`nodecore_ffi.cpp:806`) | `return false` | **always reports not-synced** — public FFI, mobile |
| `getblockchaininfo.initialblockdownload` (`blockchain_rpc_handlers.cpp:276`) | `ChainstateService::IsInIBD()` | heuristic decides mainnet IBD |
| `wallet_sync_aggregator.cpp:63` | `else` branch | header/block separation never populated |
| `chainstate_service.cpp:3634` | uses selector directly | **racy**: reads `GetBestHeader()` fields after the lock is released (see note) |

> Note on the hazard, stated precisely: `GetBestHeader()` takes `mutex_` and returns a raw pointer
> *after* the lock is released, so every field read happens outside the lock. This is a
> **use-after-free** hazard, not merely a stale read: "best header" is not a permanent property of
> an entry — a reorg can demote the former best header to a side-branch tip, and side-branch tips
> are evictable (`EvictBranch`, which runs under the selector's own mutex, not the caller's). The
> entry can therefore be freed the moment the lock is released.
>
> *(An earlier revision of this note claimed the opposite — that best-chain entries are never
> evicted so it was "only" a race. That was wrong, and is corrected here.)*

The live header-chain facts already exist in `HeaderChainSelector`, which `ChainstateService`
already holds (`header_chain_selector_`, wired at `daemon_app.cpp:2736`).

## Four states, deliberately NOT collapsed

Conflating these is the main risk in this change. `best_header != active_tip` answers only (1).

| # | State | Question it answers | Authority |
|---|---|---|---|
| 1 | **Header convergence** | Does the active chain match the best known header chain? | `HeaderChainSelector` vs active tip |
| 2 | **Initial-download policy** | Should we be downloading rather than trusting our tip? | `IsInIBD()` — network-height aware, snapshot aware |
| 3 | **Service readiness** | May RPC / wallet / mining serve requests? | `AreServicesReady()` |
| 4 | **AssumeUTXO readiness** | Is the snapshot's background validation done (e.g. prune safety)? | `assumeutxo_active_` + `IsBackgroundValidationComplete()` |

Two concrete reasons not to merge them:

- `AreServicesReady()` is `services_ready_ \|\| assumeutxo_active_ \|\| !IsInIBD()`. A snapshot node is
  **intentionally** serving while background validation continues.
- `CanPruneNow()` requires `!IsInIBD()`. Making IBD stricter (e.g. any header ahead ⇒ IBD) would
  **silently disable pruning** on a snapshot node whose headers run ahead of its validated tip.

So this change introduces convergence as a *new, separate* fact. It does not redefine states 2–4.

## Inputs available

- Header selector facts are read through copy/value accessors and purpose-built
  compute-under-lock operations. The former raw `GetBestHeader()` accessor is
  described above only to preserve the original design rationale.
- Active tip: `active_tip_` is a bare `CBlockIndex*` mutated on the chain-advancement path, so
  dereferencing it from a status reader is itself a race. `PublishActiveTip()` — already the single
  setter for that pointer — now also publishes an immutable `{valid, hash, height}` value under its
  own dedicated mutex, and the snapshot reads that. A status read never contends with block
  connection.
- `assumeutxo_active_`, `assumeutxo_base_block_`, `bg_validation_status_`.
- `ibd_network_height_` (peer-tip derived; 0 when no peer has reported), `IBD_THRESHOLD_BLOCKS = 24`.

## Behavior matrix

`conv` = header convergence (new). Values: `Converged`, `Mismatch`, `Unknown`.
**`Unknown` must never be read as synced.**

| # | Scenario | selector | best hdr | active tip | conv (new) | IBD policy (2) | Readiness (3) | AssumeUTXO (4) | `nodecore_is_synced()` |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **Cold start**, nothing loaded | null or empty | – | null | `Unknown` | true (no tip) | false | n/a | **false** |
| 2 | **Genesis-only regtest**, no peers | set | genesis | genesis | `Converged` | false¹ | true | n/a | **true** |
| 3 | **Headers ahead** (normal sync) | set | h=900 | h=100 | `Mismatch` | true | false | n/a | **false** |
| 4 | **Convergence reached** | set | h=900 | h=900, same hash | `Converged` | false | true | n/a | **true** |
| 5 | **Equal-height reorg** | set | h=900 hash A | h=900 hash B | `Mismatch`² | unchanged | unchanged | n/a | **false** |
| 6 | **Missing dependencies** (selector unset, or best hdr null, or tip null) | any missing | – | – | `Unknown` | unchanged | unchanged | unchanged | **false** |
| 7 | **Restart**, tip loaded, headers not yet re-read | set | null | h=500 | `Unknown` | unchanged | unchanged | n/a | **false** |
| 8 | **AssumeUTXO + background validation** | set | h=900 | h=900 same hash | `Converged` | false (snapshot) | **true** | validation *in progress* ⇒ `CanPruneNow()` **false** | **true** |

¹ Genesis-only regtest is not "behind" anything; this is the #429 case, already handled at the
mining gate and unchanged here.

² **Equal height, different hash is NOT convergence.** This is why the comparison must be on
**hash**, not height. Naming it `Mismatch` is deliberate: the active chain does not match the
best header chain, so the honest answer is "not converged", even though heights are equal.

### Scenario 8 is the load-bearing row

A snapshot node at convergence with background validation still running must report:
`conv = Converged`, readiness **true** (serving, as today), and prune safety **false**. If states
1–4 were collapsed, any single answer here would be wrong for at least one consumer.

## Failure-closed rule

Any missing input yields `Unknown`, and **`Unknown` never satisfies a "synced"/"ready" test.**
Rows 1, 6, 7 all exercise this. Concretely: `nodecore_is_synced()` returns true only for an
explicit `Converged`, never for `Unknown` — preserving its current conservative default while
fixing the bug that made it *permanently* false.

## Planned API shape

```cpp
// HeaderChainSelector — copied under mutex_, no pointer escapes.
// Named to match the existing GetHeaderCopy() / GetAncestorHashesCopy() pattern.
bool GetBestHeaderCopy(HeaderIndexEntry& out) const;

// ChainstateService — active tip published as a VALUE under its own mutex by
// PublishActiveTip(), the single setter for active_tip_.

// ChainstateService — the single canonical accessor consumers use.
enum class HeaderConvergence { Unknown, Mismatch, Converged };
struct SyncSnapshot {
    bool               has_best_header = false;
    uint256            best_header_hash;
    uint32_t           best_header_height = 0;
    bool               has_active_tip = false;
    uint256            active_tip_hash;
    uint32_t           active_tip_height = 0;
    HeaderConvergence  convergence = HeaderConvergence::Unknown;
};
SyncSnapshot GetSyncSnapshot() const;
```

Consumers migrate to `GetSyncSnapshot()`; none of them touch `HeaderChainSelector` directly.
States 2–4 keep their existing accessors and existing semantics.

## Out of scope (deliberately)

- **Retiring** `dinero::HeaderSyncManager` or the orphan header
  `include/dinero/daemon/header_sync_manager.h`. Separate PR: `HeaderSyncRestartRecovery` coverage
  must first be ported or explicitly replaced.
- Changing `IsInIBD()`, `AreServicesReady()`, or `CanPruneNow()` semantics.

## Verification plan

- Full regtest regression sweep.
- Mainnet **read-only** before/after comparison of `getblockchaininfo` /
  `nodecore_is_synced()`-equivalent status against the real datadir, to confirm no behavior change
  on a synced mainnet node beyond the intended `initialblockdownload` source.
