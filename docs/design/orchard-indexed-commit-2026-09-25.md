# Indexed Orchard chainstate commits

## Purpose

The service needs durable body and undo locations after a restart, in addition
to the embedded typed body, coin undo and forest delta already in ChainDB.
`ConnectIndexed` and `DisconnectIndexed` extend the owned stateful write with
that coordination. They do not enable the production connector, set chain or
script validity, publish the service active tip, or complete notifications.
All network activation switches remain unset.

## Ordering and ownership

The existing owner acquires the selected writer mutex and stages the complete
state transition. Its conventional undo is returned directly from that staging
result; the indexed adapter does not construct a second interpretation.

Under the same lock, the adapter checks persisted header metadata against the
exact candidate header, selected height/parent, stored work and CBlockIndex.
Failed headers and disagreement in status or any locator reject as local storage
errors. Existing body and undo locators must read back the exact candidate and
staged undo bytes, including flatfile framing/checksum validation.

If a new connect has no locators, it appends and fsyncs the exact body and undo.
Only then does it stage both locations and availability bits in the private
chainstate batch. Missing locations on disconnect are an error. Reconnect reuses
and verifies retained locations. No automatic repair overwrites a mismatched
indexed body or undo.

Commit rechecks the captured persisted and in-memory metadata before the write,
then synchronously commits the whole batch. Non-throwing coin/forest publication
and index availability publication follow, under the still-held writer lock.
The index, database and token must outlive the owner. The caller must prevent
reentrant writes and concurrent flatfile replacement/pruning throughout this
operation. This is not a general I/O firewall.

Abandonment or failure before commit can leave orphan append records. It cannot
make them active through database locators. A readiness error consumes the owner
without writing; once the database write starts, the existing fail-stop rule
still applies. Closing a temporary database before indexed commit is now a
readiness failure, while the non-indexed owner test still exercises the actual
write-error decision.

## Qualification scope

`OrchardIndexedCommit` uses synthetic authorized Orchard transactions, ordinary
same-block spending, real Utreexo proofs and the existing complete staged state
checks. It covers both checkpoint settings, abandonment, index disagreement at
preparation and commit, exact body/undo readback, reconnect and a validly framed
but wrong undo record or same-header body. It does not promote availability into validity.

Fresh processes cover initial connect before commit and after publication, plus
connect/disconnect readiness refusal, precommit exit and postpublication exit.
On reopen the test compares consensus rows, checks locators and restores/audits
the forest. The existing low-level crash suite retains the durable-write to
memory-publication boundary. These tests do not simulate torn sectors or certify
all filesystem power-loss behavior.

Production service routing, flatfile pruning policy, header/PoW admission,
active-tip publication, callbacks, CSN, replay/reindex and whole-daemon lifecycle
qualification remain required. This component is not a release-ready daemon.
