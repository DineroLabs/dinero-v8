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
the revision. Snapshots are bounded to 16 MiB and owned plaintext/key buffers are
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

This class stores a caller-supplied serialized state. The staged
`OrchardWalletScanState` now encodes the derived scan checkpoint and note witness
references, and restores notes from authenticated chain origins. Its production
callbacks, the note/operation reservation state machine and wallet manager/RPC
callers are still required. These tests are not a complete wallet
crash/reorg or platform qualification. Physical power loss is not simulated.

Authenticated encryption cannot detect restoration of an entire old, valid
wallet database. On open, the typed wallet layer must compare its encrypted scan
checkpoint against the selected chain before declaring balances spendable.
Backups still need a coherent wallet/chain restore procedure. Existing 8.1.12
binaries do not acquire a new wallet-open guard from this optional class.

## Typed scan state

The optional root-build `OrchardWalletScanState` consumes a fully validated
selected block and its sealed Orchard state transition. It checks exact ordered
body/authorization coverage, domain and parent continuity, the next frontier,
and the block's nullifiers. It decrypts external and internal notes, removes
spent notes and advances witnesses through every action commitment, including
padding. The old view is immutable and can be retained for rollback. Scanning
does not replace header, proof-of-work or contextual block validation.

The bounded `DNORWS01` encoding stores the domain, activation, viewing-key
identity, exact chain checkpoint and each note's origin and canonical witness.
It does not store spending keys or note openings. It belongs inside the encrypted
snapshot; note references and witnesses themselves are wallet-private data.
Restore requires the exact selected checkpoint and callbacks using that same
locked chain view. The origin callback must establish ancestor membership and
transaction inclusion from authenticated block bytes and return the verified
origin transaction. Restore decrypts its ciphertext again, checks the witness
against the selected root and tree size, and checks nullifier unspentness. Read
errors remain errors. A mismatched checkpoint requires rollback/rescan; it must
not produce a spendable wallet balance.

The scanner supports at most 4,096 currently owned nonzero notes. A limit error
preserves the previous view and must be surfaced by the eventual wallet caller.
This is a wallet resource limit, not a consensus rule. Restore currently revisits
origins for each note; large-wallet performance and pruned-node source provision
remain qualification requirements.

Address issuance and pending operation reservations are intentionally not part
of the chain-derived scan state: replacing it during a reorg must not reuse an
issued address or release a still-pending spend. Those durable wallet records
must share the outer SQLite transaction when the live wallet integration lands.
