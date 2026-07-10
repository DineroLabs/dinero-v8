# Shield-to-recipient — transparent → external `dins1` in one step (daemon)

**Date:** 2026-07-10
**Repo:** dinero-v8 (daemon). Consumers: dinero-qt "Spend privately", DineroDPI.
**Status:** design for approval, pre-implementation
**Risk:** consensus-critical (shielded note construction). A wrong note =
funds unspendable or undetectable. Gate = a passing roundtrip test.

## Problem

There is no one-step way to send **transparent** funds to someone else's
**shielded** (`dins1…`) address. Today:
- `wallet.shield` shields transparent → the caller's **own** pool (uses a
  zero diversifier + the wallet's own key; scanned by the wallet's own ivk).
- `wallet.transfer` sends **shielded → shielded** to an external `dins1`
  (via `AttachAddressedTransferInputBundle`) — but requires pre-existing
  shielded notes.

So funding an external `dins1` needs the 2-step shield-then-transfer. dinero-qt
can't even do step 2 (its "Spend privately" validates `dina1`/`dinc1`
confidential addresses, not `dins1`). We want a single `transparent → external
dins1` operation.

## What already exists (reused, not reinvented)

- `shdrv::DecodeShieldedAddress(dins1)` → `{ d, pk_d }`.
- `AddressedRecipient { d, pk_d, value_una }`.
- `EncryptNoteForRecipient(d, pk_d, plaintext)` — the sender-side note
  encryption (recipient trial-decrypts with their ivk).
- `NoteCommitment(d_packed, pk_note, value, rcm)` — addressed-output
  commitment convention.
- `AttachShieldOutputBundle` / `BuildShieldBundleForTx` — the transparent-input
  side, `value_balance = +value`, transparent change via the RPC layer.
- `BuildAddressedTransferBundleForTx(tx, spends, recipient, change, fee, memo,
  cv_bound)` — the addressed-output construction (with shielded `spends`).

Shield-to-recipient is: the addressed **output** of the transfer path, with the
transparent **input**/value-balance of the shield path, and **no shielded
spends**.

## Component 1 — ops builder

New op in `shielded_wallet_ops` / `shielded_wallet_runtime.cpp`:

```cpp
AttachShieldResult AttachAddressedShieldOutputBundle(
    dinero::Transaction& tx,
    const std::string& recipient_address,  // dins1…
    uint64_t value_una,
    dinero::WalletManager& wallet,
    const std::array<uint8_t,512>* recipient_memo = nullptr,
    bool persist = true);
```

Behaviour, mirroring `AttachShieldOutputBundle` but with an addressed output:
1. `DecodeShieldedAddress(recipient_address)` → `{d, pk_d}`; reject non-`dins`
   HRP / wrong network with a clear error.
2. Build ONE addressed recipient output = the exact construction
   `BuildAddressedTransferBundleForTx` uses for its `recipient` (fresh rcm,
   `commitment = NoteCommitment(d, pk_d, value, rcm)`,
   `encrypted_note = EncryptNoteForRecipient(d, pk_d, plaintext)`, Spartan
   output proof, Pedersen `rcv`) — **NOT** the zero-diversifier self output.
3. `value_balance = +value_una` (transparent coins entering the pool), NO
   shielded spends, NO shielded change (transparent change stays on the
   transparent side, handled by the RPC like `wallet.shield`).
4. `persist=false` for fee-probe dry runs (no pending note); `true` on the
   final build. Unlike self-shield, we do **not** persist a spendable pending
   note for ourselves — the note is the recipient's; we only need the tx.

**Extract, don't copy:** factor the addressed-output construction currently
inside `BuildAddressedTransferBundleForTx` into a shared
`BuildAddressedRecipientOutput(recipient, memo, cv_bound) -> PlannedOutput`
helper, and call it from both the transfer builder and the new shield builder,
so the encryption/commitment convention has ONE definition. (If extraction is
too invasive, duplicate with a `// KEEP IN SYNC` comment and a test that pins
both to the same bytes — but prefer extraction.)

## Component 2 — RPC

Extend `wallet.shield` with an optional `address` (+ optional `memo`):
```
wallet.shield { "amount": <DIN>, "fee_una": <int>, "address"?: "dins1…", "memo"?: "…" }
```
- No `address` → today's self-shield (unchanged, backward compatible).
- With `address` → select transparent inputs (same coin selection as
  self-shield), call `AttachAddressedShieldOutputBundle`, attach transparent
  change, sign, return `txid`. Reject a non-`dins1` address with a clear error.

(Alternatively a distinct `wallet.shieldto` method — but overloading `shield`
keeps one funding entry point and one dinero-qt/DPI call site. Decision:
**overload `wallet.shield`**.)

## Error handling
- Bad/`non-dins` recipient → `invalid_shielded_address` before any tx work.
- Network HRP mismatch (`tdins`/`rdins` on mainnet) → explicit reject.
- Insufficient transparent funds / locked wallet → existing shield errors.
- `value_una == 0` → reject.

## Testing (the gate — no merge without this)
Add to `src/test/shielded_validation_tests.cpp` (its existing
build→deserialize→validate harness):
1. **Roundtrip:** build a shield-to-recipient bundle to a known recipient
   (derive `{ivk,d,pk_d}` from a fixed seed via `DeriveShieldedAccount`);
   the consensus validator ACCEPTS the bundle (commitment, output proof,
   `value_balance = +value`, fee); the recipient's ivk **trial-decrypts the
   encrypted_note** and recovers `value_una` + `d` + rcm — i.e. the note is
   detectable and spendable by the recipient, nobody else.
2. **Negative:** a mismatched ivk fails to decrypt (privacy).
3. **Parity:** the addressed output bytes equal what
   `BuildAddressedTransferBundleForTx` produces for the same recipient/value
   (guards the shared-helper extraction / KEEP-IN-SYNC).
4. **Backward compat:** `wallet.shield` with no `address` is byte-unchanged.
5. **E2E (regtest, optional):** shield-to-recipient tx accepted into a block;
   a wallet loaded with the recipient seed sees the shielded balance.

## Out of scope
- dinero-qt UI to call this (separate task; unblocks once this ships).
- Shielded→shielded is already `wallet.transfer`; unchanged.
- Migrating self-shield off the zero-diversifier convention (tracked
  separately as "Wave 3e").
