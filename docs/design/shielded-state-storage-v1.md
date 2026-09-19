# Shielded state storage: reader and API boundary

This is the first implementation stage of the approved shielded-only storage
layout. It is not the relocation tool, an automatic schema upgrade, a recovery
API, or a production activation. Fresh stores still use the existing layout.

## Supported layouts

Legacy eight-family stores can still gain `prebase_coins` through the existing
opener; legacy nine-family stores remain unchanged. A separated store has exactly
those nine named families plus `shielded_state_v1`, schema 4, and the exact meta
value `storage_layout_v1 = shielded-state-v1:READY`.

Opening validates names and required records while retaining the real database
lock. Unknown families, unfinished layout states, a missing destination family,
missing required records, and leftover selected shielded records in `utreexo`
are refused before writable opening. No read miss selects another family.

`shielded_state_v1` contains the existing bytes of:

- `N || height_be32 || nullifier32` (37-byte keys).
- `Mshielded_frontier`.
- `Mshielded_anchor_history`.
- Optional `Mshielded_anchor_history_migrated_v1`.

`meta/shielded_tip` stays in meta. Checkpoints/checksums, replay targets,
transition proofs, deltas/undo, coins and other metadata stay in their existing
locations. The checkpoint recovery predicate remains exact U/C five-byte keys;
separation grants no broader deletion permission.

## API and caller behavior

`ShieldedStateRecord` replaces generic string-key access to the three shielded
blobs. Generic Utreexo metadata access rejects those names in either layout.
Nullifier readers/writers/deletion scans and typed records select the validated
layout's private handle. Existing caller-owned atomic batches remain intact,
including the marker and other chainstate participants. Shutdown compatibility
writes and reindex finalization retain their existing transaction boundaries;
this change does not claim to make every historical writer atomic.

For READY stores the actual daemon loader validates canonical persisted
frontier/anchors, marker height/hash/size/count and the existing shielded state
root before opening or reconciling the SQLite nullifier cache. Missing, malformed
or inconsistent state refuses startup; flat files cannot rescue it. Populated
unstamped SQLite state is preserved and refused as ambiguous, never promoted or
silently overwritten. A stamped cache is reconciled to the verified authority.
Legacy layouts retain their previous fallback behavior.

The root check reuses the existing consensus nullifier accumulator and shielded
root encoding. It currently materializes nullifier entries, like the existing
state-root path; final device resource qualification must measure this startup
cost. No new consensus rule or proof algorithm is introduced.

## Tests and remaining gates

`ShieldedStateStorage` exercises generated RocksDB layouts, named-handle order,
closed failed opens, typed access, batch visibility/abort, exact record locations
and checkpoint preservation. `ShieldedStateStartup` calls the real loader with
generated canonical data and stale external files/cache. Both run in the normal
Tests lane, using runtime assertions that remain active under NDEBUG.

Synthetic READY fixtures do not prove relocation. The subsequent
[offline ChainDB-copy engine](shielded-state-migration-engine.md) implements
database lock ownership, raw inventory and shielded-marker checks, durable
PREPARING/MOVING/VERIFYING/READY journaling, bounded verified relocation and
crash resumption on generated stores. Complete datadir ownership/eligibility
and binary/datadir rollback enforcement remain required. Full daemon import,
connect/disconnect, replay, restart/reindex, Utreexo equivalence, Linux/iOS and
real-copy resource qualification remain required.

The combined release must additionally qualify migrated nonempty databases with
production compact proof rules and the selected 60-second ASERT/reward rules.
This reader does not enable either consensus change or assign an activation
height. Older unguarded binaries must never be selected for migrated stores;
new metadata cannot make already-shipped code honor that rule.

## Separate recovery requirement: SR-1

A CF cannot survive deleting its database directory. Embedded self-heal needs a
separate core recovery API/FFI and qualified Swift callers that preserve and
validate a common shielded/chain/Utreexo recovery base, or refuse without deleting
the evidence. SST file copying is not a recovery protocol. CF relocation alone
does not satisfy SR-1; no selective reset or export/restore is implemented here.
