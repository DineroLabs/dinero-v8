# Compact canonical writer integration in progress

A distinct `OrchardCompactChainstate` owns an immutable selected catalog. It has
no full-forest or arbitrary coin lookup interface. Empty means unenrolled.
`ConnectCompactIndexed` enrolls the first state only from the private catalog of
completed independent parent replay, bound to the selected database, header,
work, profile, and retirement record. Descendants require the exact published
catalog and its retained durable record under the same writer mutex.

Compact staging shares the canonical full-node body, filter, transaction index,
Orchard state, retirement, commit journal and tip checks. It obtains candidate
inputs from the authenticated catalog and exact parent proof, runs the actual
mixed transaction validator, and applies the canonical stump transition. The
ordinary undo and delta retain the same encoding and transaction ordering as
the full-node writer. It does not read or write the full coin table or build a
full forest. The full-node path retains its existing coin and forest checks.

The successor catalog, body/undo locators and delivery outbox are staged in the
same outer write. The writer checks the live owner and captured durable records
before writing. Publication is a preallocated shared-owner swap after the
checked synchronous commit, followed by existing index publication. Durability
failure retains the existing fail-stop behavior. Abandonment or a readiness
refusal does not enroll or advance the compact owner.

Qualification is pending. New component fixtures compare against actual
full-node canonical results, remove the isolated full coin records, exercise
boundary enrollment and a descendant with modern and same-block spends, and
refuse a stale prepared commit before publication. Existing fixtures retain
their assertions and deadlines; the boundary block builder now accepts its
actual fixture height, retaining its original height-four encoding.

This is unfinished integration. Compact undo, restart enrollment, reindex,
retained-fork replay, explicit storage-mode ownership and configured consumers
still need the same owner. The production CSN guard remains, and these component
entry points do not publish a service active tip or acknowledge notifications.
The path currently requires canonical accumulator roots at both parent and
child. No mainnet height, deployment, public push or CI dispatch is enabled.
