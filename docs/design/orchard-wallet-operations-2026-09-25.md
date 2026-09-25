# Orchard pending-operation boundary

The optional `OrchardOperationQueue` is an immutable wallet-local pending queue.
It does not select coins, decide wallet ownership, relay transactions or modify
consensus. Its host must hold wallet and selected-chain locks during selection
and combine queue persistence with ordinary wallet input reservations in the
same SQLite transaction. No caller may broadcast before the ready queue commits.

## Before and after proving

`WalletBundlePlan::Intent` seals the randomized plan's real action nullifiers,
owned resolved-input context and signing message before proving. The queue
reserves every transparent outpoint and action nullifier, including padding,
under an explicit nonzero operation ID. Duplicate operations or cross-operation
input reservations fail. The caller must verify input ownership and eligibility;
an intent is not an authorization or a fresh chainstate result.

A `Reserved` entry can be cancelled before any completed transaction is exposed.
After restart, the in-memory randomized proving plan is gone. Cancel and reserve
a new plan in one wallet transaction if retrying; do not reuse its old signing
message with newly randomized effects. The original input locks stay held until
the replacement transaction commits or the cancellation commits.

`SetReady` requires both transparent and Orchard authorizations and checks the
exact reserved message, inputs and nullifiers. It freezes the complete canonical
transaction. Idempotent retry accepts only those same bytes. A ready entry cannot
be cancelled, overwritten or silently re-proved through this API. This matters
because Orchard proofs are in the transaction identity: re-proving produces a
different transaction, even for the same human payment request.

## Durable recovery

`DNOROP01` encodes the explicit domain, sorted operation IDs, phase, message,
authenticated original prevout context, nullifiers and completed bytes. It must
be stored only inside the encrypted wallet snapshot. Parsing is bounded and
checks duplicate reservations, canonical envelopes and signing-message equality.
After parsing the entire bounded input, restore re-verifies Orchard authorization
for ready entries. Persisted prevouts are authenticated wallet records, not fresh
unspentness or maturity evidence. Restored transactions still require normal
node admission before relay; restore does not mint a transparent-verification
token.

The queue caps pending entries at 128 and selected inputs per entry at 1,024.
The enclosing snapshot remains capped at 16 MiB. Failure preserves the previous
state. Production callers must report these limits; they must never discard
operations to make room.

## Qualification and unfinished integration

The component test constructs a fresh real proof and real transparent signatures,
checks reservation conflicts and message mismatches, persists both phases through
encrypted SQLite close/reopen, and brackets the ready commit with fresh-process
exits. The companion wallet row and queue recover the same side of the commit.
Malformed/truncated snapshots and edited authorization bytes reject.

This is not a complete wallet job runner. Final production integration still
needs service integration of bounded proof jobs, wallet ownership/selection, live
RPC/admission/broadcast, and selected-chain
confirmation/conflict archival with reorg resurrection. There is deliberately
no ready-entry removal API yet: those missing archival rules must not be replaced
by cancellation that could release inputs belonging to a relayed transaction.

`OrchardAccountState` now combines this queue with typed scan state and durable
address counters in one encrypted payload. Its reorg/rescan paths preserve the
queue; they do not supply confirmation/conflict archival.

The `OrchardProofJobs` component now supplies bounded execution and cancellation
against exact reserved intents. Its service integration remains unfinished;
see `orchard-proof-jobs-2026-09-25.md` for capacity and shutdown limits.
