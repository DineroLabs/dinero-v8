# Pending address capture

The three pending address consumers (`getaddressbalance`, `getaddressmempool`
and `getaddressbatch`) use one complete snapshot of immutable typed mempool
bodies, entry metadata and corresponding input coins. The capture holds the
configured selected-chain owner before the pool read lock. Each pool parent is
resolved directly from its captured body, because the spent overlay intentionally
hides outputs already consumed by children. Other inputs require checked chain
lookup; frozen pre-base inputs require live authorization before any auxiliary
lookup. An unavailable body, parent, input, configured owner or failed read
refuses the whole capture. An explicitly absent optional service is empty;
a present stopped service refuses through its operation lease.

Address computations happen on copies. Batch requests capture once for every
requested address. Confirmed scans and pending capture use the same selected-chain
lock; the history scan and existing advisory cache/proof-context behavior retain
their narrower scope. Amount arithmetic is checked, and a matching confidential
coin refuses rather than inventing a visible amount. No Historical conversion is
used by these pending consumers. The old Historical address compatibility API
remains separate and unchanged.

This is an as-of accounting snapshot, not a reservation, admission result,
complete wallet catalog, canonical history certificate or lasting membership.
It scans the present pool; resident memory/load bounds are not qualified here.
The borrowed serial component constructor can omit a chain guard; production
service configuration supplies it. External ChainDB lifetime/startup remains the
existing caller contract. An unavailable input anywhere in the captured pool
currently refuses even a query for an unrelated address. Historical confirmed
history, batch cache invalidation and proof-context consistency are not repaired
by this change. Orchard bodies are tested structurally; Orchard admission,
nullifier ownership, typed selection/mining and provider installation remain gated.

## Linux retained-file target

The completed predecessor Orchard run linked its retained-file test with typed
readers outside the Linux archive group. Their late consensus/wallet references
left actual storage/core symbols unresolved. The retained-file, typed-package
and address-capture targets now include the optional typed reader/backend and
consensus-core archives in their existing rescan group. Backend-OFF and non-Linux
library lists retain their previous behavior. Fixture assertions and deadlines
are unchanged. A Mac pass does not establish GNU archive-order qualification;
new exact Linux execution is required.

## Verification

Fresh backend-ON/OFF daemon and declared component builds, actual address RPC
component cases, existing address daemon cases and linked project-C++ sanitizer
qualification are required. Results belong in the private evidence ledger.
No unsafe-original, synchronization-removal or offensive controls are used.
Mainnet activation remains unset; this is not release readiness.
