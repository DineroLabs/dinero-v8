# Orchard account recovery from the source origin

The existing bound recovery coordinator now accepts an existing encrypted
account with no delivery receipt only when its complete scanner restores at the
exact activation-parent origin of the checked immutable source. This supports
late accounts and an explicitly persisted `RestoreForRescan` result. It does
not create missing accounts, discard a scanner automatically, or reconstruct
the ordinary-wallet baseline.

## Ownership and progress

Account discovery still authenticates every present account and referenced
archive under the selected wallet session, persistent identity and pinned key
owner. Zero-progress payloads go through the same full scanner restoration as
tracked accounts. An authenticated empty scan for a different parent, or an
untracked scan already advanced beyond the origin, cannot restore there.

Before effects, the coordinator binds the live checked source's first cursor to
the immutable replay view and checks the restored origin. No source position is
assigned by this check. The ordinary wallet and index must already have valid
progress, and all present account/archive revisions remain subject to the
existing rechecks.

An origin account participates as the lagging store, beginning with event 1.
The existing bound consumer scans the actual block with its prepared state and
verified authorizations, retaining the authenticated parent revision and
committing notes, observations and the first source receipt together. Later
partial commits are resumed using actual per-store progress. Accounts already
at the captured head are not rewound when another account joins.

An explicit rescan retains issuance, pending operations, signed bytes and archive
links through the existing account format. Recovery rebuilds derived notes and
observations by processing every retained event from the origin, including
historical transitions, undo and replacement branches. Existing archive
reconciliation and capacity refusals continue to apply. A failed first account
write leaves zero progress available for retry; it cannot acknowledge the
captured head.

## Limits

This covers existing authenticated origin accounts. Creation/key discovery,
authenticated account-inventory completeness, missing or invalidated transparent
baselines, pre-origin ordinary history and whole-wallet backup rollback remain
separate obligations. A missing account is not silently recreated. No receipt
setter, journal, identity or account-format change is introduced.

The source must cover the activation-boundary origin and all intervening
transitions, within the current 2048-record/64 MiB operational ceiling. The
canonical source and generated test histories are not independent historical
consensus certificates. Service adapters are compiled into the daemon but no
production startup/notification recovery provider is installed. Captured-prefix
completion is not wallet selection, broadcast permission or lasting readiness.

## Qualification

Fresh declared targets and the full daemon build passed. Three CTests passed:
`OrchardIndexDelivery` (79.28 s), `OrchardOperationArchive` (8.46 s), and
`OrchardAccountDelivery` (18.01 s). The default-reader-off service translation
unit compiles. Both unchanged Orchard workflow selectors still enumerate 46
enabled root tests; only these three were executed locally.

The integration regression exercises first
scan rollback, late-account recovery beyond 128 events, explicit rescan of a
real owned note and archived reservation, mismatched origin refusal, wallet
reopen/retry and later undo. No production datadir is used.

The fixture explicitly creates accounts and seeds ordinary/index baselines;
those are not independently validated historical consensus or account creation
qualification. The rescan fixture persists an explicit authenticated reset;
the recovery API never performs that reset itself. Reopen is in-process; there
is no new fresh-process or physical-power-loss claim. No new Ready-operation
proving/broadcast qualification is implied by the Reserved-operation case.

Scoped sanitizer qualification freshly instruments all 122 linked project C++
translation units with ASan/UBSan. The actual integration passes, and copied
controls restoring the single-account or enrolled-account nonzero-only guard,
or omitting live-source origin binding, fail the intended regressions. Rust and
external libraries are uninstrumented; macOS leak detection is disabled. The
service adapter is outside this sanitizer executable's scope. The separate ARM
RocksDB dependency gate remains open. Build labels/dependencies inherit the
preceding checkout and are not release-binary provenance.
