# Shielded Unshield Design (Phase 3 Wave 3c)

**Status:** Draft for review.
**Author:** Claude (sonnet 4.6 / opus 4.7).
**Last edited:** 2026-04-27.

## Goal

`wallet.unshield(amount_din, fee_una)` — spend a confirmed shielded
note so that `amount_una - fee_una` re-emerges as a transparent UTXO
controlled by the same wallet. End-to-end on regtest:
shielded note → signed v5 tx → mempool → mined block → confirmed
spend.

This is the inverse of Wave 3b shield. The hard parts are not the
cryptography (`BuildShieldedBundle` already supports the spend side);
the hard parts are the **wallet state model** for pending spends and
the **transparent envelope shape** for an unshield tx.

## Transparent envelope shape

An unshield tx has:

- `tx.version = TX_VERSION_SHIELDED (5)`
- `tx.vin = []` — no transparent inputs (the bundle's spend is the
  input)
- `tx.vout = [transparent_recipient_output]` — value = `note_value
  - fee_una`, scriptPubKey = a fresh wallet Taproot address
- `tx.lockTime = 0`
- `tx.SetExplicitFee(fee_una)`
- `tx.shielded_bundle_bytes`: bundle with one spend (the note),
  zero outputs, `value_balance = 0 - note_value = -note_value`

Validator math (existing, see `block_validation.cpp`):

```
transparent_in (= 0) - transparent_out (= note_value - fee) - fee
  = -(note_value - fee) - fee
  = -note_value
  = bundle.value_balance ✓
```

The bundle is the only "input." No transparent input signing is
needed; the bundle's spend proof + binding sig authorize the value
flow. **TransactionSigner runs over an empty `vin` list and is a
no-op for an unshield tx.**

Edge case: `note_value == fee_una` → transparent vout amount is 0.
This is a transparent dust output; we should reject at the RPC layer
with a clearer error than the dust filter. Easiest rule:
`note_value > fee_una + dust_threshold (546 una)`.

## Wallet state model — pending spends

The note store today has two states per note: `confirmed` (on-chain)
and `spent` (nullifier published in a mined block). Unshield needs a
**third state**: "spend submitted to mempool, not yet mined." Without
it, two RPCs in quick succession would happily double-spend the same
note.

### Decision: re-use `spent` flag with `spent_height = 0` as the
pending sentinel.

Rationale: no schema migration; one new transition (eviction);
re-uses the existing `MarkSpentByNullifier(nf, height)` signature.

| State | `confirmed` | `spent` | `spent_height` |
|---|---|---|---|
| Pending output (post-shield, pre-confirm) | 0 | 0 | 0 |
| Confirmed unspent | 1 | 0 | 0 |
| **Pending spent (mempool only)** | 1 | 1 | **0** |
| Confirmed spent (mined) | 1 | 1 | h > 0 |

Transitions:

- **Unshield RPC submit** → `MarkSpentByNullifier(nf, 0)`. Wallet
  selectors filter out `spent=1`, so no double-spend.
- **Block connect** (existing) → `MarkSpentByNullifier(nf, h)`. Idempotent
  re-application: pending → confirmed.
- **Block disconnect** (existing) → `UnmarkSpentByNullifier(nf)`.
  Sets `spent=0, spent_height=0`. Existing reorg path.
- **Mempool eviction** (NEW) → `UnmarkSpentByNullifier(nf)`. Triggered
  by a mempool observer hook the wallet runtime registers.
- **Daemon restart** (NEW) → at wallet runtime startup, sweep
  `WHERE spent=1 AND spent_height=0`. For each row whose nullifier is
  not in any current mempool tx, `UnmarkSpentByNullifier(nf)`. The
  mempool replay (if persistence is on) or a fresh mempool (if not)
  is the source of truth for what's actually pending.

Restart sweep is the riskiest part. If we get it wrong:
- Too aggressive (unmark a note whose tx is still in mempool) → wallet
  picks the same note for another tx → mempool rejects with
  `nullifier-already-in-mempool`. Annoying but recoverable.
- Too conservative (leave a note as pending after the tx is gone) →
  wallet permanently can't spend the note. Worse — requires manual
  intervention.

We default to "too aggressive": unmark unless we can definitively
prove the tx is still in mempool. The double-spend rejection from
mempool will surface the misclassification clearly.

## API shape

### Pure helper (consensus/shielded layer)

```cpp
// In wallet/shielded_wallet_ops.h.
struct UnshieldBundleRequest {
    consensus::shielded::Hash secret_key;
    consensus::shielded::Hash randomness;
    uint64_t                  value_una;
    uint64_t                  leaf_index;
    consensus::shielded::CommitmentTree::AuthPath auth_path;
};

struct UnshieldBundleResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash nullifier{};
    consensus::shielded::Hash anchor{};
    uint64_t bundle_bytes = 0;
    std::string error;
};

UnshieldBundleResult BuildUnshieldBundleForTx(
    dinero::Transaction& tx,
    const UnshieldBundleRequest& note_input,
    uint64_t fee_una);
```

Mirrors `BuildShieldBundleForTx` from Wave 3b. Pure. Caller
responsibilities BEFORE invoking: tx.version=5, single transparent
recipient vout already populated, locktime + explicit_fee set.

Generates `PlannedSpend` with cv/rcv/range proof and calls
`BuildShieldedBundle({planned_spend}, {}, tx_sighash, &bundle)`.
`value_balance` derived inside builder = `0 - note_value`. Caller
must already have computed `tx_sighash` invariance: bundle
serialization is appended last.

### Wallet wrapper

```cpp
struct UnshieldAttachResult { ... };

UnshieldAttachResult AttachUnshieldInputBundle(
    dinero::Transaction& tx,
    uint64_t              note_leaf_index,
    uint64_t              fee_una,
    dinero::WalletManager& wallet);
```

Looks up the note from the wallet's note store, builds the auth path
from the wallet-side `CommitmentTree`, calls
`BuildUnshieldBundleForTx`, marks the note pending-spent. Returns
nullifier + anchor + bundle size for RPC response.

### RPC

```
wallet.unshield(amount_din, fee_una?)
  → { status: "unshielded", txid, nullifier_hex, value_una, fee_una,
       transparent_recipient, bundle_bytes, inputs: 0 }
```

amount_din selects the smallest unspent confirmed note >= amount.
If no such note exists, `insufficient_shielded_balance`. (We do not
yet support spending multiple notes in one bundle — that's Wave 3d
"transfer." Wave 3c is one-note-only.)

`transparent_recipient` is a fresh wallet address from
`wm.getNewAddress("unshield-out", "taproot")`. The user does not
specify a destination — it's always self.

## Test matrix

1. **Happy path** (`test_shielded_rpc_unshield_e2e.sh`):
   shield 1 DIN → mine → `wallet.unshield 1.0` → tx in mempool →
   mine → tx in block → `wallet.shieldedbalance` shows 0 notes;
   `wallet.balance` (transparent) shows the recovered amount minus
   fee. ~30s on regtest.
2. **Double-spend rejection**: shield → mine → unshield → submit
   second unshield for same amount immediately → expect mempool
   rejection (no available unspent note OR `nullifier-already-in-
   mempool`).
3. **Insufficient balance**: with no shielded notes, call
   `wallet.unshield 1.0` → expect `insufficient_shielded_balance`.
4. **Wrong envelope binding**: build an unshield bundle for tx_a,
   splice it into tx_b with different prevouts → bundle's binding
   sig fails verification → expect `BindingSigInvalid` from block
   validator. Constructed at the C++ unit-test layer (cheaper than
   shell), not the RPC layer.
5. **Restart preserves state**: shield → mine → unshield → mine →
   restart daemon → `wallet.shieldedbalance.tree_size` and
   nullifier_count unchanged; spent note still has `spent=1,
   spent_height=h_mined`. (Existing daemon-restart equivalence
   tests cover this exact path with the integration `shielded_tx_
   builder` — extending one of them to use `wallet.unshield` is the
   minimum delta.)
6. **Reindex preserves state**: shield → mine → unshield → mine →
   `--reindex-chainstate` → daemon comes back up with the same
   shielded tip marker, tree size, nullifier set. Existing
   `ShieldedReindexEquivalence` already covers reindex; adding an
   unshield tx to its fixture asserts the spend side too.

Negative tests 2, 3 are cheap — add to the same shell test as the
happy path. Test 4 lives in `shielded_validation_tests.cpp`. Tests
5, 6 extend existing fixtures rather than adding new files.

## Build order

1. `BuildUnshieldBundleForTx(tx, request, fee_una)` in
   `src/wallet/shielded_wallet_ops.cpp`. Pure. Unit test in
   `shielded_validation_tests.cpp` (mirror of
   `BuildShieldBundleForTx` tests): builds a synthetic envelope
   with a transparent vout, attaches the bundle, asserts the full
   `ValidateShieldedBundle` path returns `Ok` with
   `transparent_value_delta = -(note_value - fee)`. Negative test
   for envelope-binding (test 4 above).
2. `AttachUnshieldInputBundle(tx, leaf_index, fee, wallet)` in
   `src/wallet/shielded_wallet_runtime.cpp`. Wraps step 1 + marks
   note pending-spent.
3. **Mempool eviction hook** + **wallet startup sweep** for the
   pending-spent state machine. Smaller than it sounds: mempool
   already has `onTransactionRemoved` callbacks (search `mempool.cpp`
   for the hook surface); we add a callback that walks the bundle's
   nullifiers and calls `UnmarkSpentByNullifier`. Startup sweep is
   one SQL query + a mempool query.
4. `rpc_wallet_unshield` in `src/rpc/shielded_rpc_json.cpp`. Mirrors
   `rpc_wallet_shield`: parse params, select note, build envelope,
   attach bundle, sign (no-op since vin empty), submit. ~80 lines.
5. Integration test `test_shielded_rpc_unshield_e2e.sh`. Happy path
   + tests 2, 3.
6. Memory update + checkpoint.

Estimated work: 1-2 days. Step 3 is the only place where I'd
expect surprises (mempool callback wiring); everything else is
mechanical mirror-of-Wave-3b.

## Out of scope (explicitly)

- **Multi-note unshield** (e.g., user has 0.3 + 0.4 + 0.5 DIN notes,
  wants to unshield 1.0). Wave 3c spends exactly one note. If the
  user has no single note >= amount, RPC returns
  `insufficient_single_note`. Multi-note coin selection is Wave 3d
  ("transfer / consolidate") which also covers shielded → shielded.
- **Change**: spending a 1-DIN note to unshield 0.3 DIN is also
  Wave 3d. Wave 3c spends the entire note value.
- **Memo / encrypted_note for the spend side**. Spends don't carry
  user-visible metadata in our scheme.
- **Selecting non-self transparent recipient**. The unshield always
  goes to a fresh self-controlled wallet address. Sending shielded
  funds to an external transparent address is a transfer + unshield
  combination, also Wave 3d.

## Open questions for review

1. **Pending-spent sentinel**: re-use `spent_height=0` as proposed,
   or add a separate `pending_spent_txid` column? Sentinel is
   simpler; explicit column is more debuggable.
2. **Eviction-source-of-truth**: trust mempool callbacks, or run
   the startup-sweep + mempool reconcile every N seconds? My
   default is callbacks-only; runtime sweep is over-engineered for
   the regtest-and-soak phase we're in.
3. **Self-recipient address derivation**: `getNewAddress` (BIP86
   external chain, increments index) vs change chain (index 1).
   Default to external chain so it shows up in normal address
   listings; this is the user's money returning to them.
4. **Locking the note immediately at RPC vs. after `submitTransaction`
   accepts**. Lock-after-accept is the simpler path and matches what
   transparent `sendtoaddress` does (UTXO is only mempool-spent
   after acceptance, not optimistically). Keep as proposed.
