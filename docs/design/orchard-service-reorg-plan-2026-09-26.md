# Typed reorg transaction handoff

## Problem and behavior

The historical reorg reconciliation collected `Transaction` values and skipped
unreadable blocks. A mixed body cannot use that representation. Best-chain
activation and explicit invalidation now prepare an owned plan before rollback
or CSN forest changes whenever the disconnect or replacement path uses Orchard.
Each entry retains the exact typed body, height and hash. A missing or inconsistent
body refuses the entire attempt; it never yields a partial transaction list.

The reader checks contiguous parent paths, indexed availability/status/work and
locators, selected-format body identity and required witness commitments. Its
64 MiB encoded-byte and 2048-block defaults are operational preparation limits,
not consensus rules. A larger reorg requires a future streaming preparation
path or an explicitly qualified operational limit change. They are not exact
resident-memory bounds because parsed bodies also occupy memory.

## Consumer and progress contract

Per-block readiness does not establish reorg readmission readiness. The new
`PrepareReorg` entry defaults to refusal. A production consumer must durably
retain the plan before returning a prepared handle. It must recover committed
progress from canonical chainstate on startup: a process can stop between a
block commit and its notification. Merely retaining a pointer is insufficient.

The transition scope records successful disconnects in tip-first order and
connects in ancestor-first order. Its no-throw completion distinguishes a fully
completed plan from interruption. On an interrupted walk the counts are only a
confirmed lower bound: historical code may throw after durability but before
returning. The consumer must resolve canonical progress even without a process
restart, and must never cancel or discard an intent merely because those counts
are zero. Only an explicitly completed walk certifies its final counts. Readmission
reverses only the committed disconnect prefix, and independently revalidates
transactions against the selected current chain. The plan is an identity-checked
input, not an admission certificate. Coinbase entries are retained in the body
for exactness and must never be submitted as ordinary transactions.

Best-chain activation skips its old transaction collection for this prepared
path. Explicit invalidation completes its handoff before a separate replacement
activation, so transactions stay recoverable even when no replacement connects.
The legacy-only path retains existing behavior.

## Scope

This change wires the preparation and progress contract into actual service
orchestration. It does not install a production consumer, durable outbox, Orchard
mempool admission or wallet scanner. Those remain required; without a provider
that implements reorg preparation the operation refuses before rollback.

The generated-store service tests exercise exact mixed bytes, complete-plan
refusal, ancestry and bounded preparation, real disconnect/reconnect commits,
zero-progress cancellation and partial/full progress. They do not establish a
running-daemon multi-peer reorg, restart recovery of a production outbox, or
cross-boundary historical consensus replay. Mainnet activation remains unset.
