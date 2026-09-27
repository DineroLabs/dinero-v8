# Taproot import persistence owner

The actual descriptor import RPC now calls one checked wallet operation before
starting its existing rescan. `storeTaprootKey` binds the private key to its
internal key, tweaked output and selected-network address, then commits the
imported address, watched script, public-key mapping and key record in one FULL
SQLite transaction. The RPC passes its captured wallet session; a reopen or wallet
switch before persistence refuses the import. The real database lease owns the
wallet lifecycle and SQLite connection throughout persistence.

Required reads, bindings, writes and commit are checked. Caller-owned transactions
are refused unchanged. Encryption policy is read from durable settings and
metadata and checked against the pinned live owner. Encrypted imports require a
usable unlocked owner; encryption errors refuse. Existing incompatible watch,
mapping, key or address ownership refuses. Imports remain explicit non-HD account
`-1` entries and use valid `p2tr` address rows with exact script bytes. Reimport can
update the explicitly supplied label/key material only for matching public keys;
it does not silently overwrite a different owner. Existing compatible watch scan
metadata remains intact. Existing key schemas without the encryption flag are
migrated inside the same transaction.

The live index is updated after COMMIT. A publication exception can therefore
return failure with a complete durable import retained for retry. There is no
cross-store receipt or all-consumer acknowledgment. Existing delivery invalidation
guards still apply. The older standalone public registration API and P2MR/import
surfaces are outside this change; the descriptor RPC no longer uses the split
Taproot registration sequence.

## Qualification

Four cases in the actual WalletManager test executable exercise complete imports,
multiple imported keys, reimport/reopen/index reload, each required write failing,
deferred COMMIT failure, caller transaction preservation, conflicting watch paths,
mismatched keys, stale sessions, encrypted/locked owners and durable-before-live
publication. Full daemon builds compile the real RPC caller. This does not claim
execution of that RPC or its rescan, signature production from the imported key
lookup path, mempool acceptance or broadcast. These tests were added with the
implementation; there is no original-source-test-first claim.

Fresh declared backend-on/off builds, actual test counts, sanitizer translation-unit
maps and copied fault-control results are recorded in the private qualification
receipt. External/Rust libraries are uninstrumented in project-only sanitizer
qualification; the ARM full-RocksDB gate remains open.

## Remaining obligations

This is a forward import persistence owner, not an authenticated global account or
issuance catalog. Historical incomplete imports, orphan/conflicting metadata,
complete script/key discovery, watch-only and nonzero HD accounts, backup rollback,
other descriptor/P2MR owners and pending/reservation ownership remain unresolved.
The broader imported-key encryption lifecycle and decrypt-and-sign lookup still
need qualification.
The existing RPC rescan is not a prepared immutable source or a readiness proof.
The provider is absent; first-activation bootstrap and release qualification remain
open. No activation, cursor reset, new journal or automatic account recreation is
introduced.
