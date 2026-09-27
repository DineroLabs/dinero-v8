# Checked P2MR inventory reads

P2MR address listing previously could return empty or partial results when a
query failed or a stored public field could not be decoded. The actual
`ListByWallet` now checks the requested owner, SQL preparation and binding,
every selected row's stored types and field lengths, and the terminal
`SQLITE_DONE`. A malformed later row or an interrupted query discards the whole
result. The component RPC handler returns `StoreError` with no partial entries.
Existing ordinary RPC callers of the list API propagate its exception through
their existing error paths.

`OpenExistingReadOnly` opens only existing storage, without database creation or DDL.
Missing/unreadable storage and missing schema refuse instead of becoming an
empty inventory. The `wallet.listp2mraddresses` JSON adapter uses this path.
Other P2MR write/open callers retain their existing behavior. In particular,
address issuance and initialization have not been made atomic across the main
wallet database and the separate P2MR store.

The list is scoped to the requested wallet ID and reads in one SQLite query.
It preserves recorded address/path/label/time bytes and the full uint32 leaf
index. Nullable labels remain supported. Ciphertext/nonce/tag lengths are
checked but their authentication is not performed by the public listing.
There is no key derivation, historical script substitution, data repair,
account creation, master-key initialization, new journal, or cursor change.

## What this does not certify

A successful read describes present rows for the requested owner. It cannot
prove that deleted keys, rows, accounts or backup history never existed. It
does not authenticate seed-to-public-key, public-key-to-script, derivation-path
ownership, ciphertext, or other wallet IDs. It must not authorize creation of a
missing PQ master. Unlock publication and explicit initialization versus
recovery ownership remain separate work. Existing historical imported addresses
must retain their exact original tweak and script identities.

## Validation

The new declared `WalletP2MRInventory` test uses actual P2MR creation and listing
component handlers with public synthetic material. Three cases exercise a
missing file, unrelated existing schema, nonconsecutive owner IDs, read-only
write refusal and byte preservation; malformed later rows with no returned
prefix; and a SQL-free `sqlite3_interrupt` at a row callback followed by a retry
on the same connection. Tests and implementation were added together; there is
no original-source red-test claim. Existing store/AEAD and handler integration
targets also run. The JSON adapter is compiled, not executed by these cases.

These component checks do not qualify RPC transport, production key ownership,
full startup, crash/power loss, pending/admission/broadcast, Orchard activation,
whole-node or release behavior. Mainnet activation remains unset.
SQLite WAL sidecar behavior is not separately qualified by these rollback-journal
fixtures; byte-preservation assertions cover the existing database file.

## Local qualification

Fresh genuine backend-ON and backend-OFF configurations built the full daemon,
wallet database test target and inventory target. Five affected CTests passed
in each configuration. Existing P2MR storage/AEAD and handler targets passed
40 and 28 assertions respectively. Both unchanged Orchard selectors enumerate
46 enabled registrations; those are inventory checks, not 46 local executions.

After the final header documentation refinement, both declared daemon/test
builds and inventory executions were repeated, followed by fresh ASan/UBSan
compilation of all ten project C++ translation units linked into the inventory
binary. All three cases passed; 1228 source/header hashes stayed unchanged.
Three copied controls omit read-only opening, terminal-query checking, or
cipher-field length validation. Each fails an intended assertion and the
restored binary passes. Sanitized/control maps contain no project C++ archive
members. The normal discovery map retains its original archive members to
identify the complete linked graph.

The daemon JSON adapter, wallet manager regression binary, external libraries,
project C and PQClean are outside this sanitizer scope. macOS leak detection is
off; the full ARM RocksDB sanitizer gate remains open. Prebuilt OpenSSL and
inherited build labels are not release provenance. No compile or incidental
test failure occurred; intended controls are not production exploit tests.
