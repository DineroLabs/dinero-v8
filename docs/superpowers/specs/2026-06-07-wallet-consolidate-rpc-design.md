# Design: `wallet.consolidate` daemon RPC

**Date:** 2026-06-07
**Repo:** `dinero-v8` (daemon), branch `feature/wallet-consolidate-rpc-v8`
**Status:** IMPLEMENTED — daemon RPC landed and both regtest tests green on this branch. (This doc was authored against the obsolete `dinero`/`p2p-fix` tree; internal references to that repo/branch are historical — the live work is here on `dinero-v8`.)
**Client follow-on:** `dinero-qt` (repoint the existing dead Consolidate button) — deferred until this merges.

## Problem

The dinero-qt wallet ships a fully-built "Consolidate" button (receive tab) whose
handler `onConsolidateUTXOs()` fires `rpc_->callNamed("consolidate", params)`. The
daemon registers ~95 `wallet.*` RPCs but **no `consolidate` handler exists**, and the
method name is not namespaced like every other wallet RPC. Clicking the button always
fails with JSON-RPC `-32601 method not found` → "Consolidation Failed". The feature was
never wired backend-side.

Consolidation is a wallet maintenance operation: spend many small UTXOs back to the
wallet's own fresh address, collapsing them into one larger UTXO to reduce future fees
and improve spend performance. It is a normal transaction (N inputs → 1 self output)
with policy guardrails.

## Architecture decision

Daemon owns the operation; clients call the RPC. Coin selection belongs beside the
wallet's UTXO set, lock-state, fee policy, signing rules, dust rules, and address
derivation — not duplicated in the UI/app. This matches the Bitcoin-Core model:
daemon owns wallet operations; UI/CLI/mobile call RPC.

New handler `rpc_context_wallet_consolidate` lives in
`src/rpc/methods_wallet_context.cpp`, registered as `wallet.consolidate` (with a
`consolidate` alias) and added to the `src/daemon/http_rpc_server.cpp` method
allowlist. It **reuses the existing, proven machinery** from
`rpc_context_wallet_sendtoaddress` (`methods_wallet_context.cpp:2405+`): the same
UTXO-filtering chain, fee estimation, signing, and broadcast path. No new
coin-selection or signing code path is invented.

### Reused infrastructure (verified present)

- `wallet_service->get().listUnspentUTXOs(min_conf, max_conf, mempool)` → vector of
  `WalletManager::WalletUTXO` with fields: `spendable`, `is_mature`, `is_coinbase`,
  `is_confidential`, `confirmations`, `amount_una`, `txid`, `vout`, `derivation_path`,
  `script_pubkey`.
- Existing send-path exclusion chain: mempool-spent, locked (`isUTXOLocked`),
  immature/coinbase, confidential, and not-present-in-live-utreexo-forest
  (`WalletUtxoIsPresentInLiveUtreexoForest`). This satisfies the "never consolidate
  frozen/locked/immature/staked/reserved" guardrail with **no new flag plumbing**.
- Fee estimation: mempool fee estimator, `wallet.txfee_din_kb` override, min-relay
  floor of 1 una/byte on BIP141 vsize.
- Fresh address derivation: `wallet_service->get().getNewAddress(label, "taproot")`
  for P2TR (BIP86 `m/86'/1448'`); P2MR (BIP88 `m/88'/1448'`) routes through the
  registered `wallet.getnewp2mraddress` handler via `g_rpcRegistry.lookup(...)`.
- `broadcast` flag semantics already established in the send path (preview vs send).

## Scope

**In scope (v1):** transparent consolidation of the live v7 surface — Taproot (P2TR)
and P2MR (post-quantum). One family per transaction, no mixing.

**Out of scope (v1):**
- **Shielded / confidential.** Parked in v7 (no UI; send path excludes confidential
  UTXOs). `address_type:"shielded"` or any confidential input → explicit error
  `"shielded consolidation not supported in v7"`. Surface is wired to add later.
- **Caller-supplied destination.** v1 always consolidates to a fresh self-derived
  address. A "sweep/consolidate to cold wallet" feature with a caller-supplied
  destination is a separate, deliberately differently-named future feature so it is
  never mistaken for normal wallet consolidation.

## Request schema

```jsonc
wallet.consolidate {
  "address_type":       "p2tr|p2mr|auto",  // default "auto"
  "max_inputs":         100,                // hard cap; clamped to policy vsize/standardness max
  "min_confirmations":  6,                  // default 6
  "include_unconfirmed": false,             // default false
  "fee_rate":           "auto",             // "auto" | number (una/vB)
  "max_fee_percent":    1.0,                // abort if fee > 1% of selected value
  "max_fee_din":        1.0,                // abort if fee > 1 DIN (whichever trips first)
  "dry_run":            true,               // default true — preview unless explicitly false
  "broadcast":          false               // default false — broadcast only when true
}
```

Defaults are conservative: **preview-by-default** (`dry_run:true`), **no broadcast**
unless explicitly `broadcast:true`.

## Behavior — one family per transaction

1. **Gather & filter** spendable UTXOs via `listUnspentUTXOs(min_conf, …)`, applying
   the existing exclusion chain (mempool-spent, locked, immature/coinbase,
   confidential, not-in-live-utreexo-forest). `min_confirmations` is the floor for
   confirmed inputs. `include_unconfirmed:false` (default) means only inputs with
   `confirmations >= min_confirmations` are eligible; `include_unconfirmed:true`
   additionally admits wallet-owned 0-conf (mempool) outputs. Default false because
   consolidating unconfirmed inputs risks building on a tx graph that may not confirm.
2. **Partition by family**: P2TR vs P2MR, by `script_pubkey` prefix
   (`5120<xonly>` = Taproot, `5320<xonly>` = P2MR) corroborated by `derivation_path`
   (`86'` vs `88'`).
3. **Pick the target family**: `auto` → the family with the most eligible UTXOs;
   explicit `p2tr`/`p2mr` → that family only. **No mixing**: one call consolidates
   exactly one family into exactly one output.
4. **Select** up to `max_inputs` eligible UTXOs from the target family.
   Selection order: **largest-count-first, dust-aware** — the goal is to reduce UTXO
   *count*, so take as many eligible inputs as fit under `max_inputs`/policy vsize.
   (Approved alternative not chosen: smallest-value-first dust-sweep.)
5. **Derive a fresh self address** of the target family as the single output
   (BIP86 for P2TR, BIP88 for P2MR).
6. **Build** the self-send transaction; **estimate fee** (`fee_rate:"auto"` → mempool
   estimator, with min-relay floor). `output_value = input_value - fee`.
7. **Fee sanity gate (BOTH modes):** reject if
   `fee > max_fee_percent% of selected input value` **OR** `fee > max_fee_din`.
   - Dry-run: return the plan with `ok:false, fee_ok:false, reason:"…"`.
   - Execution: **hard-abort before signing/broadcast** (never sign a rejected plan).
8. **Dry-run** (default `dry_run:true`): return the plan, no signing.
   **Execute** (`dry_run:false`): require the wallet unlocked, sign, then:
   - `broadcast:true` → broadcast and return `txid`.
   - `broadcast:false` → return signed `rawtx`/hex **without** broadcasting.

## Response

```jsonc
// dry-run (plan accepted)
{ "ok": true, "dry_run": true, "address_family": "p2tr",
  "selected_inputs": 47, "input_value": 123.456,
  "estimated_fee": 0.0123, "output_value": 123.4437,
  "destination": "din1p…(fresh)", "fee_ok": true, "txid": null, "rawtx": null }

// rejected by fee gate (dry-run still returns the plan)
{ "ok": false, "dry_run": true, "fee_ok": false,
  "reason": "fee 1.2 DIN exceeds max_fee_din 1.0",
  "selected_inputs": 47, "input_value": 123.456, "estimated_fee": 1.2 }

// executed + broadcast
{ "ok": true, "dry_run": false, "broadcast": true,
  "address_family": "p2tr", "selected_inputs": 47,
  "input_value": 123.456, "fee": 0.0123, "output_value": 123.4437, "txid": "…" }

// executed, not broadcast (signed hex returned)
{ "ok": true, "dry_run": false, "broadcast": false,
  "address_family": "p2tr", "selected_inputs": 47,
  "fee": 0.0123, "output_value": 123.4437, "txid": null, "rawtx": "0200…" }
```

## Edge cases & guardrails

- **Empty wallet / no eligible UTXOs in family** → `ok:true, selected_inputs:0`
  (nothing to do; not an error).
- **Single UTXO in the family** → `ok:true, selected_inputs:0`,
  `reason:"nothing to consolidate"` (1→1 only burns fee).
- **`max_inputs` clamp**: clamped so one tx cannot exceed policy vsize/standardness
  limits — one call cannot create a giant policy-invalid transaction.
- **Unlock requirement**: `dry_run:false` (signing) requires an unlocked wallet;
  checked *after* the fee gate, *before* signing. Dry-run never needs keys.
- **No mixing**: confidential/shielded inputs are filtered out entirely; P2TR and P2MR
  are never combined in one tx.

## Test plan (daemon, regtest)

Tests MUST use throwing / exit-nonzero assertions (not bare `assert()`, which is a
no-op under `NDEBUG`). Each test fails without the new handler.

1. **Empty wallet** → `ok:true, selected_inputs:0`.
2. **Single UTXO** (one family) → no-op, `selected_inputs:0`, reason set.
3. **Many dust UTXOs** (happy path) → consolidates, `output_value ≈ input − fee`.
4. **Mixed P2TR + P2MR** → `auto` picks the majority family, output is that family,
   the other family's UTXOs are untouched (never mixed).
5. **Locked coins excluded** — lock a UTXO via `wallet.lockunspent`, confirm it is not
   selected.
6. **Immature coinbase excluded** — a freshly-mined coinbase is not selected.
7. **Fee gate trips on percent** — force a high fee_rate so fee > `max_fee_percent`;
   dry-run returns `ok:false, fee_ok:false`.
8. **Fee gate trips on absolute DIN** — fee > `max_fee_din`; rejected.
9. **Dry-run returns plan without signing** — no tx created, no keys touched.
10. **`dry_run:false, broadcast:false`** — returns signed `rawtx`, nothing in mempool.
11. **`broadcast:true`** — produces a txid that confirms when a block is mined.

## Client follow-on (in scope, final phase — `dinero-qt`)

Repoint the dead button (`src/mainwindow.cpp`):
- `onConsolidateUTXOs()` (`:13641`): call `callNamed("wallet.consolidate", …)` with the
  new param schema, `dry_run:true` first to populate the confirmation dialog with the
  real plan (`selected_inputs`, `estimated_fee`, `output_value`).
- On user confirm, issue a second call with `dry_run:false, broadcast:true`.
- Update success handler (`:6720`) and error handler (`:7061`) to read the new response
  fields (`address_family`, `selected_inputs`, `fee`, `txid`, `fee_ok`/`reason`).
- Method name change also fixes the un-namespaced `consolidate` → `wallet.consolidate`.

## Non-goals

- Multi-family single-call consolidation (callers loop per family if desired).
- Shielded/confidential consolidation.
- Caller-supplied or cold-wallet destinations.
- Automatic/background consolidation triggers (the `shouldConsolidateUnderLoad` privacy
  heuristic is unrelated and untouched).
