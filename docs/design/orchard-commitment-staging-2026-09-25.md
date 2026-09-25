# Composite commitment enforcement in staged chainstate

Status: full staged stateful connector; no production admission, activation or
release claim. Partial coin-only and Orchard-only helpers remain partial.

## Connect

The full connector validates mixed coins/proofs/filter/state, reads the exact
selected parent and requires one DNRS v2 coinbase output with the projected
composite root. Missing, duplicate, wrong-version and wrong-root commitments
reject the candidate. A bad persisted parent commitment is a local storage
failure, not peer consensus invalidity. Parent bytes are independently decoded
and authenticated to their header before comparison.

At the first boundary, the caller must explicitly supply its independently
validated historical retirement receipt; omission fails. In particular, this
API does not derive or authenticate the retired monetary amount or selected
epoch from history. Never use a wallet display, network payload or arbitrary
RPC field as that source. Implementing that selected-history accounting source
is still required before any production caller can use this adapter. This is
not a cryptographic certification type merely because its argument is named
`authenticated_boundary`.

The connector independently compares the receipt's network/activation and
boundary parent and rederives legacy tree root/size, unique nullifier content
with epoch heights, canonical anchor contents and the full historical SHR1
from ChainDB. It requires matching tip markers. Descendants take the same
frozen receipt from storage and cannot provide a replacement boundary input.
The Orchard pool starts empty and never receives the retired amount.

Retirement receipt, exact undo and advancing legacy tip marker share the same
outer batch with coins, conventional undo, Orchard state/undo, forest delta and
checkpoint, body/indexes, compact filter, journal and selected tip markers.
Failure returns an empty batch; success still does not commit. Caller holds
the selected writer lock, commits once and then publishes memory, and must
append no legacy-content writes after this function. Header/PoW, runtime
ownership, service/flatfile coordination and ordinary CT compatibility remain
separate unfinished obligations.

## Disconnect and startup

Before staging rollback, check the current DNRS and frozen legacy contents.
Project the exact stored Orchard undo without writes; above the first boundary,
compare its logical parent state to the authenticated stored parent coinbase.
Retirement undo and legacy marker restoration join the same reverse batch.
Boundary rollback removes the retirement receipt and returns to the preserved
legacy marker, without payout, subtraction or Orchard credit. Tip-local startup
audit runs the same checks on an abandoned batch and repairs nothing.

Checks scan existing logical sets and legacy contents. They do not audit every
older owner/undo record, establish historical proof validity, or qualify loaded
capacity. Snapshot imports and pruned parent-body availability still need an
explicit authenticated runtime path.

## Tests

Synthetic stores now carry real serialized legacy frontier/anchor state and
its computed SHR1, with a deliberately synthetic retired amount (37 una).
Mixed candidates include DNRS before calculating their coinbase txid/forest,
so tests exercise real Merkle/forest/proof obligations without an own-hash cycle.
Coverage includes four commitment rejection forms, missing boundary input,
changed retirement amount, frozen anchor mutation, boundary/descendant undo,
restart audit and fresh-process pre/post-commit/publication boundaries. These
fixtures are not historical mainnet accounting evidence.
