# Orchard wallet persistence boundary

`WalletSnapshotStore` is an optional persistence component. It borrows the
existing wallet's SQLite connection and never opens a wallet, selects a datadir,
commits the outer transaction, or publishes in-memory wallet state.

## Atomicity and protection

The caller holds the wallet lock and begins a transaction. Schema creation and
`StageReplace` require that transaction, `synchronous` FULL or stronger, and a
disk-backed WAL/delete/truncate/persist journal. One encrypted snapshot holds the
whole Orchard wallet state for an account; the caller stages ordinary wallet
records in the same transaction. It publishes memory only after commit succeeds.

Snapshots use AES-256-GCM with a fresh OS-generated nonce for each replacement.
The key is HKDF-SHA256 from the caller's unlocked wallet seed, separated by a
wallet-storage domain, network, genesis, opaque wallet ID and account. The same
identity and the monotonically increasing revision are authenticated with the
ciphertext. No private key, seed or decrypted snapshot is written by this class.
An existing snapshot must decrypt successfully before it can be replaced.
Account rows are independent. Wrong keys, relocated ciphertext, edited revisions
and schema versions other than one are errors, not empty-wallet results.

Writes compare the expected revision with the current row. A chain rollback
writes a new revision containing the restored wallet state; it does not decrement
the revision. Snapshots are bounded to16 MiB and owned plaintext/key buffers are
scrubbed on release. Caller copies, library internals and swap are outside that
scrubbing claim. Large wallets will need explicit performance/size qualification;
a bound failure must never discard the existing state.

## Tests and scope

The temporary-wallet test writes an encrypted snapshot and a companion wallet
row in one SQLite transaction, closes and reopens the database, checks rollback,
and uses fresh child processes that exit immediately before or after commit.
Reopening recovers both sides together. Separate cases check account separation,
wrong keys/networks, modified ciphertext/revisions, stale writers, future schema,
inadequate durability settings, empty state and the size bound.

This class stores a caller-supplied serialized state. The typed snapshot codec,
selected-chain scanner, note/operation reservation state machine and wallet
manager/RPC callbacks are still required. These tests are not a complete wallet
crash/reorg or platform qualification. Physical power loss is not simulated.

Authenticated encryption cannot detect restoration of an entire old, valid
wallet database. On open, the typed wallet layer must compare its encrypted scan
checkpoint against the selected chain before declaring balances spendable.
Backups still need a coherent wallet/chain restore procedure. Existing8.1.12
binaries do not acquire a new wallet-open guard from this optional class.
