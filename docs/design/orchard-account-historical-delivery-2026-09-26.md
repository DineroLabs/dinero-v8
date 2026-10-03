# Orchard account delivery below activation

An Orchard account must follow every retained transition when a reorg crosses
below activation. `ApplyHistoricalDelivery` consumes the genuine historical body
from the selected checked delivery source. It advances the existing account
receipt together with its empty scan checkpoint and pending-operation conflict
observations. It creates no second receipt or journal.

The account must already have rewound its Orchard scan to an empty checkpoint.
Each event checks the contiguous sequence and predecessor digest, selected
domain and activation, direction, height, before-tip hash, exact body encoding,
header identity, Merkle root and duplicate transactions/inputs. A connection
moves the empty scan to that historical block and records actual spends of
transparent inputs reserved by pending Orchard operations. A disconnect moves
to the parent and removes observations above it. No balance, notes, witness,
issued-address counter, signed Ready bytes or operation archive is silently
retired. At the activation parent the ordinary typed Orchard scanner can resume
against its actual next prepared state and verified authorizations.

These checks bind identities and account effects. The event remains a POD and
must come from the selected checked source. They do not independently establish
historical consensus validity, script execution, source digest authenticity,
pre-origin baseline completeness or current node readiness. Historical CT
compatibility in the transparent store consumers remains unresolved; this
change does not authorize losing those funds.

## Persistence and restore

DNORAC05 keeps the existing encrypted payload layout and delivery receipt but
allows pending-input conflict observations below activation. Versions 01–04
remain readable with their original observation-height restrictions. A legacy
format cannot assert a new historical conflict. Raw scan changes still refuse
tracked accounts, and an explicit rescan still clears the receipt instead of
claiming completion.

Before activation, a scan checkpoint describes the wallet's empty pool at the
actual applied historical tip. It is not a newly persisted consensus Orchard
state. Restore requires a zero pool and empty frontier below activation;
nonempty note records cannot belong below their creation height. Historical
conflict observations require a selected historical-body lookup. The exact
body must reproduce the observed transaction and spent input; missing or wrong
history refuses restore.

`RestoreOrchardAccountFromChainUnderLock` supports this historical case only
when the canonical and validated tips agree and no Orchard state is stored.
It derives the empty wallet checkpoint from that tip and uses actual archival
bodies for historical observations. Selected height indexes, header ancestry,
body hashes and Merkle commitments remain mandatory. The existing activated
case retains its selected Orchard checkpoint and input-origin verification.
The adapter is read-only and its caller still owns the chain snapshot and
independently validated database prerequisite.

The account remains an immutable staged value. Persist it with the existing
WalletSnapshotStore outer checked FULL SQLite transaction before publication.
This does not supply authenticated parent-scan retention, safely bound runtime
key ownership, initial baseline adoption, or full production recovery/provider
installation. Those obligations remain for the account owner.

## Qualification scope

Fresh declared account, operation-archive and chain-restore executables and the
full daemon build pass; the runtime-reader-off service translation unit also
compiles. All three corresponding CTests pass, including the final
strengthened account lane. The source fixtures carry genuine body identities,
proofs and account effects, but are not independently validated historical
consensus. The account tests include return through the typed activation scan,
wrong historical bodies, legacy payloads, preserved Ready bytes/address counters,
and two additional fresh-process exits immediately before/after SQLite COMMIT
for historical empty-checkpoint and conflict states. The actual ChainDB adapter
is checked for read-only reopen, stale tips/indexes, and missing/corrupt historical
observation bodies. Parent-snapshot persistence and full node crashes remain open.

All 29 linked project C++ translation units of the account executable and all
71 of the chain-restore executable were freshly instrumented with ASan/UBSan;
these counts overlap and are not a sum of unique source files. Link maps exclude
project C++ archive members. Rust and external libraries remain uninstrumented,
and macOS leak detection is off. The ARM RocksDB dependency sanitizer gate is
unchanged. Four copied-account-source omission controls test historical tip
movement, conflict recording, conflict undo, and Merkle validation. Each fails
the intended assertion without sanitizer diagnostics, and restored source passes. No original-
source test-first or release-binary provenance claim is made; local labels and
prebuilt OpenSSL dependencies are inherited.

Both unchanged workflow selectors retain all 45 enabled root Orchard
registrations; not all 45 were executed locally. This account/restore component
does not install production notification readiness or safely bind runtime seed
ownership. Mainnet activation remains unset.
