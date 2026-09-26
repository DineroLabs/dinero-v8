# Staged legacy retirement receipt and undo

Status: storage component for the future Orchard connector. No live caller,
activation, balance derivation or migration is enabled by these APIs.

## Receipt and selected tip

`LegacyRetirementRecord` stores network/genesis, branch, boundary height and
parent, legacy epoch height, historical composite SHR1, retired una, and legacy
tree root/count and nullifier count. The default network and branch are invalid.
No observed wallet amount is a constant. This record describes the pool being
retired at the new boundary, not every earlier discarded epoch or total supply.

`LegacyRetirementState` adds current height, block hash and parent hash. The
frozen record is identical on all descendants; current chain markers continue
to advance. A connect requires the exact committed parent receipt, consecutive
height and explicit parent linkage. Initial creation is allowed only at the
record's boundary, with no existing or orphaned retirement rows. The current
validated tip and legacy tip marker must match the selected parent.

The marker binds legacy tree root/counts. It does **not** prove SHR1 or the pool
amount. The full connector must authenticate the receipt from validated legacy
state/history, compare its selected network and activation configuration, and
bind it into the new composite DNRS root. Loading this receipt must never turn
an unauthenticated historical estimate into an authenticated balance.

## One outer transaction

The separated shielded column family reserves `R1S` for the current receipt and
`R1U || block_hash` for exact before/after undo. Records use fixed-width,
little-endian integers, raw 32-byte hashes and strict `DLR1`/`DLU1` framing.
Current records are 237 bytes; undo is 242 or 479 bytes. Trailing/truncated bytes,
unknown versions, invalid fields and inconsistent undo reject.

Staging writes the receipt, undo and advancing legacy tip marker together. The
caller commits once with Orchard, coins, forest, canonical/validated tips and
indexes in the same ChainDB batch. A savepoint preserves the caller's prior
batch on failure; abandonment commits nothing. Existing legacy-content/marker
writes in that batch are rejected. Orchard writes are allowed before or after
this stage, and their independent pool guard still starts Orchard at zero.
Retired value is never a fee, coinbase reward, UTXO or Orchard credit.

The caller holds the writer lock throughout and must not append legacy-content
writes after retirement staging. This API is not a general WriteBatch firewall;
freezing actual frontier/nullifier/anchor contents remains an obligation of the
complete connector and its startup audit. Ordinary historical paths are unchanged.

Disconnect requires the exact selected current receipt and a valid matching
undo. It restores the previous receipt and legacy marker. Undoing the boundary
removes the current receipt and restores its recorded pre-boundary marker.
A later boundary crossing on another ancestry must derive a new record. There
is no new finality rule, permanent checkpoint or payout on disconnect.

## Qualification and remaining integration

Generated-store tests cover frozen fields, full record/marker round trips,
reopen, repeated transitions, stale/mismatched context, another boundary branch,
legacy writes in the outer batch, orphan/corrupt records and parent-link damage.
Atomic companion tests create and undo Orchard in either staging order while
retired value remains isolated and transparent coins unchanged. Separate
processes exit before/after the synchronous outer connect/disconnect commit.
These are process-loss boundaries, not a hardware power-loss experiment.

The fixtures deliberately use opaque synthetic legacy roots and frontier data.
They qualify storage lifecycle only, not historical accounting or full state
validation. Composite DNRS construction, authenticated retirement derivation,
real ConnectTip/DisconnectTip/reindex/CSN callers, snapshots and wallet display
are still required before activation or release.
