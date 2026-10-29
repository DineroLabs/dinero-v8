# Ordinary wallet source delivery

`RuntimeOrdinaryDelivery` applies real historical and Orchard transparent
transaction effects to the selected WalletManager database. Its `DNOW01`
receipt commits in the same checked SQLite transaction as UTXOs, transaction
history, confirmations, maturity and the ordinary tip height. This gives the
coordinator an independent applied cursor when the separate index has committed
but the ordinary wallet has not. It does not acknowledge note/account stores,
vault observers, the mempool or other consumers.

## Ownership and source

The public API requires the intended live wallet session, pins the actual wallet
lease, and obtains the existing persistent database identity. That identity
transaction is separate; it can remain initialized after a delivery failure.
The existing checked source reader must supply the event before wallet ownership
is acquired. Shared exact typed decoding extracts actual IDs, inputs and outputs,
including same-block spends; no Block or Transaction shells are constructed.
Confidential historical outputs refuse before effects. This restriction does
not resolve CT compatibility or authorize retiring ordinary confidential funds.

The receipt binds the persistent identity, the union of wallet address scripts
and watched scripts, watched derivation paths, selected source profile, source
sequence/digest, origin and resulting tip. Contiguous events must match the
previous digest and pre-transition tip. Exact replay is idempotent. First
adoption requires source event one, rejects rows ahead of the origin and rows
belonging to another wallet ID, but does not certify baseline completeness or
branch correctness. The event remains a POD: this consumer does not independently
certify source digests, consensus history or wallet key ownership.

## Ordinary effects and failure handling

Connection checks every SQL preparation, binding and execution. Owned inputs
are marked with spending transaction and height. Owned outputs are inserted in
transaction order with exact integer amounts and scripts; creation replay
preserves existing spend metadata. Unknown display encodings retain the owned
output with its exact script and an empty address. Existing addresses provide
the display text. Receive/mining history retains existing send/self-spend history
semantics, and source block time supplies deterministic timestamps. Transaction
history uses the existing REAL display amount column; spendable amounts remain
integer UNA. Optional labels and external observers are outside this receipt.

Disconnect removes outputs/history created at that height and restores the
actual input outpoints, clearing spend metadata. Derived confirmations and the
ordinary tip height share the transaction. Coinbase maturity retains the
existing ordinary wallet's 100-confirmation display rule, not a new consensus
rule. The in-memory manager height is not published by this API; all-store
readiness and coordinated memory publication remain the owner's obligation.

The existing lease selects and verifies synchronous FULL before BEGIN. A failed
BEGIN does not adopt a caller transaction. Effect, receipt or COMMIT failure
rolls back the owned transaction, including newly added receipt columns and
invalidation guards. An active rollback failure terminates. No cursor-only
setter or reset-to-ready API exists.

Guards on ordinary UTXOs, history, address/watch ownership and tip/rescan metadata
invalidate progress in the same transaction as legacy mutations. Successful
source application replaces progress only after completing its effects. Reads
check the exact guard definitions. Invalidation retains a tombstone even when
all rows have been removed. Reopen preserves progress; unrelated ordinary writes
require reconciliation. These checks provide local consistency, not protection
against arbitrary database editing. Schema/ownership scans have not been load
qualified.

## Qualification scope

The mandatory OrchardIndexDelivery integration lane now exercises both actual
stores over checked generated source events. It checks output/receipt/deferred
COMMIT rollback, caller transaction preservation, an index-only committed
prefix followed by ordinary retry, same-block spends, exact replay, reopen and
stale session refusal, skipped sequence, failed disconnect delete/restore,
historical down/up transitions below activation, reconnect, missing guards and
ordinary-write invalidation. Generated bodies and seeded baselines are not
independently validated historical consensus or a running node.

Qualification receipts record fresh declared builds, sanitizer scope and source
omission controls. Full production recovery/provider readiness, note/account
progress integration, late-account/rescan reconciliation and independently
validated activation history remain unfinished. Mainnet activation stays unset.

Fresh declared CMake integration and full daemon builds pass, as does the
runtime-reader-off service translation unit. The final actual
`OrchardIndexDelivery` CTest passes; its existing registration now includes the
ordinary-store scenarios above. Both unchanged workflow selectors require 45
enabled root Orchard registrations; not all 45 ran locally.

All 113 linked project C++ translation units were freshly built with ASan/UBSan.
After strengthening the receipt/schema assertions, the changed test unit was
rebuilt and the final lane rerun. Four copied-source controls omit ordinary
receipt writing, transaction ownership, invalidation enforcement or source
ordering; each fails an intended assertion, and restored source passes. Final
maps contain no project C++ archive members. Rust/external libraries are
uninstrumented and macOS leak detection is off. No original-source test-first,
fresh-process, physical power-loss, whole-node or release-binary provenance
claim is made. Local labels are inherited from the parent configuration.
