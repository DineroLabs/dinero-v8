# Orchard account recovery from selected archival state

## Scope

`RestoreOrchardAccountFromChainUnderLock` replaces caller-created test callbacks
with a synchronous read-only ChainDB adapter. It restores an already decrypted,
authenticated account snapshot at the current validated Orchard checkpoint.
It is a staged component; wallet startup and RPC callers are not enabled.

The caller holds chain and wallet snapshot locks throughout. The selected database
must already have passed startup audit or replay. Checking a height index and
block contents here does not independently establish accumulated work, historical
unspentness or complete consensus validity. A stale or divergent wallet snapshot
requires retained undo or an explicit rescan; this API never promotes it to the
current height.

## Checks

- ChainDB tip, validated tip and Orchard checkpoint agree. Selected header
  metadata, height indexes, header hashes and immediate parent links agree.
- A transaction index is only a locator. The selected block must contain the
  exact requested transaction at that ordinal.
- Post-activation bodies use the typed stored-block reader, including transaction
  and witness commitments. Historical prevout bodies have their transaction
  Merkle root recomputed; only txid-bound output data is used from those bodies.
- Previous amounts, scripts, creation heights and coinbase flags come from the
  original bodies, including inputs no longer in the current UTXO set. A
  same-block source must precede the receiving transaction. Unsupported
  confidential prevouts are rejected, not converted to explicit amounts.
- The receiving transaction's Orchard proof/signatures and native transparent
  signatures, maturity and contextual locks are verified again. At most one
  authorization result is cached within this call and locked selected view.
- A nullifier owner row must name a selected block containing that nullifier.
  Missing ownership is distinct from a database error or malformed row.
- Restoration preserves pending transactions and issued-address counters and
  performs no database writes. Fresh node admission is still required before
  broadcasting any restored Ready operation.

## Storage contract and remaining integration

This adapter requires active transaction-index coverage and archival original
bodies. Historical bodies may use the archival BlockStorage reader; new typed
bodies currently come from ChainDB's staged body storage. Post-activation flatfile
and pruned-node recovery are not implemented. It is not a wallet rescan driver,
stateless recovery path, consensus replay, or repair tool. The runtime domain and
branch selection remain host obligations.

`OrchardChainRestore` uses temporary RocksDB stores and real synthetic proofs.
It covers close/reopen with already-spent funding inputs, historical and ordered
same-block funding, unchanged database rows, preserved account state, stale
transaction/height indexes, altered and missing historical bodies, malformed
nullifier owners, mismatched checkpoints and closed-database errors. These stores
are authenticated-content fixtures, not mined/full-consensus chain fixtures.
Root CI requires this registration and actual execution alongside the existing
Orchard tests. Release qualification still requires live node lifecycle tests.
