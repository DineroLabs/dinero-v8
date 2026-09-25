# Ordered mixed-block coins and atomic staging

## Coin authorization and accounting

`PrepareOrchardBlockCoinsUnderChainstateLock` walks the complete typed candidate
in block order using one owned overlay over the authenticated parent coin view.
Spent entries remain tombstones; they cannot fall back to an unspent parent row.
An earlier transaction's outputs are available to a later child. Neither the
parent view nor ChainDB changes during preparation.

Orchard transactions use the combined transparent/Orchard verifier against that
overlay. Ordinary transactions use the shared `ValidateSpend`, contextual lock
and fee routines. Legacy shielded and confidential lanes are excluded from this
staged new-pool path. Historical callers and replay rules are unchanged.

Coin amounts and sums are bounded. Coinbase maturity, duplicate inputs, duplicate
transaction identities and output collisions are checked. The result owns both
ordered spend/create records and the net persistent coin changes. Outputs created
and spent within the block remain in the ordered records for forest processing,
but have no persistent row and no external-coin undo entry.

The fee total combines the exact mandatory Orchard fees with derived ordinary
transparent fees. The shared coinbase comparator receives that same total.
An underclaim burns the difference; it is not credited to the Orchard pool.
Ordinary transparent transactions have no net pool flow.

## ChainDB staging

`StageOrchardBlockCoinsAndStateUnderChainstateLock` resolves the authoritative
database view, prepares the ordered coins, then stages Orchard state, nullifiers,
anchors, net coins and conventional coin undo in one caller-owned batch. It
requires an empty batch on entry. Validation/storage failure rolls back its
staged writes. No helper commits or publishes live state.

The disconnect adapter authenticates the selected tip's body and checks that
conventional undo covers exactly the body's net created and externally spent
outpoints. It checks current created-coin contents, then stages coin restoration
and Orchard undo together. Same-block parent outputs are never resurrected.
Conventional undo is retained for reconnect and must match the new preparation.

The caller MUST keep the branch/coin writer lock, validate the remaining block
rules, append forest, forest undo, tip and index changes to this same batch, commit
once, and only then publish in-memory state. These adapters are stateful; they do
not authenticate CSN input metadata or replace Utreexo validation.

## Tests and remaining integration

Tests use real authorization fixtures, a freshly signed ordinary child, and
temporary RocksDB stores. They cover ordering, conflicts across transaction
families, maturity, signature failure, reward bounds, source-view isolation,
database errors, batch abandonment, late undo mismatch, exact disconnect,
reopen and reconnect across the first Orchard block.

These are component/storage tests, not a daemon lifecycle qualification.
PoW/header context, block resource limits, forest proof/application, actual
ConnectTip/DisconnectTip and replay/reindex callers, mempool/relay/mining, wallet
operations and platform qualification remain required. Mainnet activation is
unset and no live admission path has changed.

Native macOS qualification: the daemon builds; all 12 registered root Orchard
lanes plus five existing Merkle/witness lanes pass (17 total). ASan/UBSan passes
for the combined staging test with all 42 project C++ translation units selected
by its link map instrumented. External dependencies and Rust are uninstrumented;
macOS leak detection is disabled. Earlier partial-instrumentation failures and
the corrected link-map qualification script are retained in private evidence.
Linux Actions must qualify the exact source separately.
