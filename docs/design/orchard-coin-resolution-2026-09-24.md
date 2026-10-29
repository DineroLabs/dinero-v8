# Staged Orchard coin resolution

The typed reader now has an owned coin-resolution companion. It reads each
transparent input exactly once through `consensus::ChainStateView`, using the
wire txid and output index, and retains the full `UTXOEntry` in input order.
There is no wallet/RPC lookup, separate `hasCoin` probe, caller-supplied amount,
or caller-supplied signing digest in this API.

The snapshot owns the exact immutable envelope as well as its coins. Its
Orchard authorization method derives the signing context from these copies;
later view changes cannot change that context or substitute another envelope.
Missing inputs and database I/O/corruption errors retain distinct source
statuses. Resolution publishes no partial result. Confidential value metadata
is unsupported, and individual and aggregate explicit amounts are bounded.

## Lock and admission contract

`ResolveUnderChainstateLock` requires the host's chainstate lock or an immutable
authenticated view. The existing `ChainStateView` interface has no snapshot
token or lock ownership API. A changed-height check catches one broken caller
contract; it cannot detect a same-height reorg and does not provide locking.
The host must maintain the same state context through validation and atomic
application, or discard the result and resolve again. Never cache this object
as a reusable proof of unspentness across chainstate changes.

This is **coin resolution and Orchard authorization**, not full transaction
admission. Coinbase maturity, relative/absolute locks, transparent witness and
script signatures, Orchard anchor/nullifier state, and atomic pool/coin updates
remain separate gates. Height and coinbase metadata are preserved for those
gates, not treated as already checked. The synthetic fixture's scripts and
witnesses are not valid-spend evidence. No runtime admission caller is enabled
and no activation parameter changes.

Transparent authorization must bind the complete Orchard intent. The existing
historical transparent sighash cannot simply be reused on a converted legacy
`Transaction`, which would discard the new bundle context. Supported witness
forms and their complete signing construction still need implementation and
review before this snapshot becomes an admission input.

## Qualification

- A separately registered `OrchardCoinSnapshot` test exercises the actual
  adapter against a deterministic synthetic implementation of `ChainStateView`.
- The saved valid Orchard bundle verifies with the resolved fixture coins.
  Changed scripts and redistributed input amounts (same aggregate) change the
  signing digest and reject authorization. A changed aggregate rejects at the
  independent balance check. Outpoint byte order and indices are pinned.
- Tests cover missing first/later coins, database failures, changing view
  height, copied metadata, view mutation after resolution, confidential values,
  money bounds and zero-input resolution. None claims real-chain provenance.
- macOS Release: all four standalone CTest entries and all five selected root
  entries pass, including the eleven Rust cases. Root links the real consensus
  core. This is not a full-daemon/full-suite qualification.
- ASan/UBSan passes with the new test, resolver and C++ Orchard wrapper/signing/
  envelope sources instrumented. The pinned Rust archive and historical codec
  archive are not instrumented in this run; macOS leak detection is disabled.
- CI requires the new executable and exact four/five-entry inventories. Linux
  results for this source must be checked separately before merge.

The first local test attempt expected to obtain a digest after changing the
aggregate input value. It correctly failed at the existing balance guard. The
test now checks that rejection separately from the same-total input mutation;
no validation rule was relaxed.
