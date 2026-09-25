# Selected legacy epoch accounting

`DeriveSelectedLegacyPoolAccountingUnderLock` is a read-only archival component.
It derives the public accounting value to retire at the selected Orchard
boundary. It does not enable a daemon caller or change any activation height.

## Source and calculation

The caller supplies an already validated selected ChainDB and holds its writer
lock for the complete call. Selected network parameters must remain fixed.
Both tip markers must name the exact activation parent; the selected genesis
must match the network. The scan starts at the latest historical epoch reset
before that parent, or the original shielded activation if there was no reset.
Reset-block shielded activity is rejected as inconsistent historical input.

Each epoch body is checked against the selected height/header, its parent link
and recomputed transaction Merkle root, including duplicate-tree detection.
Funding prevouts come from their original selected bodies. The transaction
index is only a locator: identity, output index and prior height/order must
match. Earlier transactions in the same block are supported. Today's UTXO set
is not a substitute for historical inputs, which may already be spent.

For each legacy shielded envelope the delta is:

    transparent input amounts - transparent output amounts - explicit fee

Accumulate this delta from zero at the epoch boundary, checking monetary bounds
and nonnegativity at each step. The bundle's value-balance claim and wallet data
are not amount sources. Fees must be present. This relies on historical full
validation having enforced the fee/coinbase and spend rules; this adapter does
not independently establish those historical obligations.

The scan selects the shielded transaction version, not bundle emptiness. It
requires the bundle to be committed in the transaction identity. Unsupported
historical formats or confidential monetary inputs/outputs produce an explicit
unavailable result (`Status::Invalid`), not zero. Missing, malformed or
inconsistent source records remain local lookup errors, not peer invalidity.
These restrictions do not change the historical transaction acceptance rules.

## Integration contract and limits

The result binds epoch, parent height/hash, amount, epoch-body count and
shielded transaction count. Funding-origin body reads are additional work. There is no
cached amount across reorgs and no partial success. A caller-selected block
budget must cover the complete epoch; it is local work policy, not consensus.
Storage is read one block/origin at a time; work still scales with epoch length
and referenced inputs. Loaded-node and archival capacity are not qualified.

The caller must bind the result to the same selected frozen legacy state when
constructing the retirement record. Full staged commitment checks then verify
that record's content against local frozen state and stage it atomically. This
new amount source is used by the selected boundary factory described in
`orchard-retirement-boundary-2026-09-25.md`; production ChainstateService remains
unwired. It does not certify snapshot provenance, historical validity,
past proof soundness, full supply reconciliation or previous retired epochs.
A validated-tip marker alone cannot establish those properties.

This work does not recover notes, search for holders, carry balances into
Orchard or alter transparent funds. Orchard still starts at zero. The existing
issuance RPC remains unchanged.

## Qualification

`OrchardLegacyAccounting` is a mandatory root-build CTest registration. Generated
stores cover public deposit/withdrawal/fee arithmetic, same-block funding,
reopen with no writes, competing selected suffixes, resets, original activation,
missing/corrupt bodies, stale locators, wrong genesis/tips, monetary bounds,
duplicate inputs and explicitly unavailable formats/amounts/fees. These fixtures
exercise real storage and serialization but do not contain valid historical ZK
proofs or qualify full replay. Linux CI must run on the committed source.

Local qualification includes the full daemon build, the targeted generated-store
suite, 47 selected CTests (32 mandatory Orchard registrations and 15 shared
compatibility lanes, including 21 internal Rust tests), and ASan/UBSan on 21 linked project C++ translation units. Rust and
external libraries are not instrumented; macOS leak detection is disabled.
Copied-source controls that omit explicit-fee accounting or whole-body Merkle
checking each fail the regression; the restored source passes. The original
origin-body mutation alone was also caught by its txid check, so qualification
adds an unreferenced coinbase mutation to isolate the whole-body check.
