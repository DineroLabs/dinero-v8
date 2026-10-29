# Account restoration before coverage reconciliation

The coverage reconciliation service prepares an opaque `OrchardCatalogCapture` before taking selected-chain ownership. The capture authenticates the complete declared account catalog, current and reached archive envelopes, all retained account/archive payloads, viewing keys and ordinary shield history in checked FULL SQLite. After that read commits and releases its wallet and seed owners, the capture fully restores current accounts and linked parents against a retained immutable replay view.

The capsule has no public constructor and exposes no mutable account or serialized validity flag. It retains the exact replay shared owner, authenticated bytes, wallet instance and session. It is not enduring spend authority. The existing service selected-head check still runs before the writer. Inside the ordinary write transaction, the complete capture is authenticated again and compared byte-for-byte, including revisions, catalog and derived keys. This account preflight performs no scan restoration or replay callback. A mismatch or failed read refuses before index effects. Index-first/ordinary-second commit ordering and partial-prefix retry behavior remain.

Preparation refuses caller-held wallet leases and transactions. The production service already refuses entry under its selected mutex. Tests use a narrow friend to wrap real replay point/origin lookups and observe released owners, and SQL-free same-thread authorizer observations in the real service caller. They also cover wrong replay/session, catalog growth, borrowed transactions, final-read refusal before index effects and a successful retry. No concurrent mutation or synchronization-removal control is used.

## Receipt-prefix observations

While the coverage projection is built, its existing traversal checks each ordered event and compares the post-event block hash and height against the immutable replay checkpoint. It retains that exact cursor (including its digest), hash and height, along with the validated profile digest. Retained prefix values are charged to the existing operational material limit. This is a logical retained-data charge, not a complete resident-memory bound.

The ordinary and index receipt checks preserve framing, checksum, wallet identity, script-domain decoding, profile, origin, cursor and tip comparisons. They now use the projection's exact prefix facts, with no replay Event or Point lookup inside their write owners. A zero, out-of-range or wrong-digest cursor has no matching fact. Distinct disconnect and reconnect cursors remain distinct even when their block hash and height match. A captured projection cannot recognize later prefixes.

The prefix fixture compares every retained fact against actual replay checkpoints before and after an actual disconnect/reconnect, checks altered digests and future cursors, and verifies that prefix reads preserve the wallet and index snapshots. The existing coverage cases are unchanged.

## Remaining scope

This migrates the account-restoration portion of coverage reconciliation. Account recovery loops and other account writers remain separate work. Complete lazy source verification, all-caller detachment, general-history resident-memory bounds, platform/load qualification and release readiness are not established here. Mainnet activation remains unset.

## Commit-phase fixture qualification

The preparation capture and ordinary receipt writer have separate checked commits. The ordinary commit-refusal fixture arms only on preparation of the actual ordinary receipt UPDATE, allows the prior capture commit and still requires a retained index prefix plus fresh-session reopen/retry. A separate first-commit refusal case requires both stores unchanged and a successful retry. The historical recognition fixture observes both successful commits before live map publication. Assertions on retained owners and store bytes remain. Qualification results are recorded separately; this prepared source is not a pass claim.
