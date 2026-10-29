# Archived operation reactivation in coordinated recovery

The replacement-branch extension is described in
`wallet-archive-branch-replay-2026-09-26.md`. The ancestor-only behavior and
qualification below describe the preceding implementation.

## Behavior

The account recovery coordinator now uses `OrchardAccountDelivery::ApplyForReplay`.
Under the existing selected-wallet session, persistent identity and recovery-key
owner, it restores the authenticated account and reconciles every referenced
operation archive before and after applying the actual source event. Account
receipt, parent retention and restored pending reservations share one checked
FULL SQLite transaction. A failed reactivation rolls back that account event;
previously committed index/ordinary/account prefixes remain available for retry.

Reconciliation follows all authenticated archive pages. An operation already in
the fully restored pending queue retains its current bytes and observation. An
archived cause still on the selected branch stays archived. When the selected
checkpoint is an actual ancestor of the removed cause, the existing
`StageReactivate` reacquires reservations, including its capacity and conflicting
reservation checks. The archive remains intact. Parent links, issued addresses
and operation history are preserved. The reused archive operation preserves Ready
bytes; this integration regression exercises a Reserved operation.

`RuntimeAccountReplay` supplies immutable selected hashes and ancestor checks
from its retained branch graph. These callbacks acquire no chain locks under
wallet ownership. Missing ancestry refuses. The graph is selected source replay
material, not independent historical consensus certification.

The coordinator also reconciles accounts already at the captured source cursor.
This repairs an older owner's missing reactivation when that cursor is an
ancestor of the archived cause, without inventing a source receipt or advancing
height. Repeated reconciliation leaves already restored reservations unchanged.
Both explicit-account and all-enrolled-account recovery use the new path; the
low-level Connect/Disconnect/Historical APIs retain their narrower behavior.

## Required refusals and remaining work

An older owner may already have advanced onto a replacement branch without
reactivating an archived operation. Merely restoring reservations there could
miss an earlier conflict on that replacement branch. This path refuses with
`Orchard archive branch reconciliation required`; complete observation replay
for that case remains work. Normal ordered undo restores reservations before
subsequent replacement-branch blocks observe them.

This is not baseline enrollment, a complete account catalog, or spend/broadcast
readiness. Missing pre-origin history, late-account/rescan reconciliation,
account deletion before discovery, the replay capture ceiling, configured
non-wallet consumers and production provider installation remain unresolved.
No activation or deployment changes are included.

## Validation scope

The actual integration fixture uses a real WalletManager, index, ordinary store,
encrypted account and authenticated archive over the checked canonical reader.
It covers a failed reactivation update after an account undo was staged, unchanged
account receipt/revision on failure, ordered retry, historical transitions,
reopen, reconnect observations, stable issuance/archive, an unchanged connected
cause and already-applied ancestor recovery with idempotent retry. Its generated
bodies and explicitly seeded baseline are not independently validated history.
The source-view tests also check historical cursor hashes, unavailable heights
and actual ancestor identities. No running production recovery provider is
installed by this change.

### Local qualification

A fresh declared CMake build of the affected executables and full `dinerod`
passed. The final three CTests passed: OrchardIndexDelivery (58.97s),
OrchardOperationArchive (8.42s), and OrchardAccountDelivery (17.90s). The actual
service translation unit also compiled with its runtime-reader macro removed.
Both unchanged Orchard workflow selectors enumerate 46 enabled root tests;
only the three tests above were executed locally for this change.

All 122 project C++ translation units linked into the integration executable
were freshly instrumented with ASan/UBSan. Its link map contains no project C++
archive members. Rust and external libraries are uninstrumented; macOS leak
checking is disabled. The service adapter is outside that sanitizer scope. The
separate ARM RocksDB dependency finding remains open.

No original-source test-first execution, new fresh-process test, physical power
loss, installed-provider lifecycle or release-binary provenance is claimed.
The existing archive suite's process-boundary tests were rerun. Build labels
inherit the preceding commit and external dependencies retain their own scope.

Three instrumented copied-source controls omitted post-effect reactivation,
omitted caught-up reconciliation, or returned the tip hash for the wrong branch
height. Each failed its intended regression without sanitizer diagnostics; the
restored executable passed again. Omitting post-effect reactivation failed the
unchanged account-revision assertion after a later retry hit the injected SQL
error, demonstrating the required transaction boundary.

The initial C++ build found friend-only account access in an anonymous helper;
the helper became a private nested owner rather than exposing metadata APIs.
The control harness initially overwrote its build-path variable, then expected a
different assertion for the atomicity omission. These harness issues were
corrected without changing production checks or regression assertions.
