# Durable Orchard spend requests

## Behavior

`QueueCatalogRequestForReplay` binds a caller request ID to the exact ordered
Orchard receivers, amounts, 512-byte memos, transparent output scripts and
amounts, and explicit fee. The host computes a domain-separated SHA-256 digest
with explicit field widths and counts. It also binds the authenticated
persistent wallet ID, network, genesis, branch, activation, account, request ID,
and this API's fixed locktime. Session and current revision are intentionally
excluded so an authenticated retry can identify its original request after
reopening. The digest alone is not authority: it resides inside the existing
encrypted account/operation archive, restored under its real key owner.

New work uses the existing full-catalog reconciliation, expected revision,
selection, change issuance, plan, capacity-before-reservation and checked
COMMIT path. Its request commitment is part of that same reservation write.
There is no separate request journal or post-commit binding write.

A retry first fully restores every declared/current/reached/retained account.
It searches the current operations and existing authenticated archives across
all accounts. A foreign-account ID, unbound legacy operation or mismatched
request refuses. A matching operation returns its saved intent or signed bytes
and any stored observation after a checked read COMMIT, without reconciliation
writes, new randomness, new selection, executor lookup or another submission.
The original revision may be stale on this read path. Current entries take
precedence over their retained archive record after reactivation; both bindings
must agree. Missing, stopped or restarted proof execution never grants release
or regeneration of the durable reservation.

The return value distinguishes a saved request from a task published by this
call. `enqueued=false` on a retry says nothing about whether another task is
running or whether signed bytes were exposed. Observations are those in the
restored account/archive, not a fresh chain-readiness certificate.

## Encoding and preservation

Pending queue DNOROP02 adds a strictly typed optional 32-byte nonzero commitment
per entry. DNOROP01 still restores as explicitly unbound. Queues containing no
bound requests retain their exact legacy encoding; there is no implicit
migration. Unknown flags, zero commitments, truncation, trailing data and a
noncanonical version-2 encoding with no bindings refuse. Existing immutable
Ready handling and archive continuity checks preserve the commitment together
with the intent and transaction bytes.

## Qualification status

Locally qualified with fresh full ON/OFF declared targets;77 ON and72 OFF selected CTests passed plus the enabled ON OrchardRuntimeReader. All linked project C++ freshly ASan/UBSan:176 replay,122 wallet,35 state-machine cases and five separate ordinary DaemonApp process modes. Fourteen new ON/two OFF owner/request cases executed. Exact source/input/maps and private evidence independently verified. External/Rust/C/PQClean uninstrumented; macLSanoff; normal9regression binaries/reader/OFF and production main outside sanitizer graphs. No publicsend/shield RPC or release readiness; mainnet unset.
Eight ordinary ON cases and one OFF policy case executed through
an independent 180-second CTest/CI lane. They cover exact retries, an absent or
stopped executor, healthy reopen, changed request fields/order, legacy and
foreign owners, SQL/read-COMMIT refusal, initial write/COMMIT rollback, strict
encoding, Ready, archive completion and selected disconnect/reactivation.
Existing fixtures, assertions and deadlines remain unchanged. No unsafe
race/churn, synchronization-removal or original reproduction controls.

Fresh parent-based ON/OFF daemon and declared targets, complete linked sanitizer graphs and independent private evidence verification completed as scoped above. This does not implement a public
send/shield RPC, restartable randomized proof plans, automatic proof collection,
admission/relay, cancellation policy or release readiness. Mainnet is unset.

## Archive fixture boundary

Mining and checked replay retain an observed current operation. The existing
`OrchardOperationArchive::StageCompleted` API explicitly moves that operation to
the authenticated archive. The request retry fixture exercises that API before
requiring an archived result, then verifies reactivation after actual disconnect.
The current-versus-archived assertions and exact signed-byte checks remain in
place. Production replay and archive policy are unchanged by this fixture repair.
