# Canonical outbox replay context

Delayed account recovery needs the scan checkpoint and validation inputs for an
intermediate branch, including one that has since been disconnected. The active
Orchard undo row is removed on disconnect. A current-tip lookup cannot replace
that missing historical view.

New indexed typed writes retain DNOE03 in the existing canonical delivery log.
The sealed write captures the actual prepared parent/next Orchard checkpoints,
exact canonical coin undo, and the successful branch-MTP lookup answers used by
block coin validation. These fields share the body, sequence, predecessor and
checksum of the outbox record, in the same full canonical batch. No wallet amount
or derived account checkpoint supplies this data.

The same batch maintains a block-hash locator to its retained connect event.
Disconnect checks that locator against the retained event's cursor, direction,
profile, exact body, before/after checkpoints and coin undo, then reuses its
validation-time MTP answers. A reconnect checks those facts before replacing the
locator with the new connect sequence. The locator is an index into the existing
outbox, not a second journal or a completion receipt.

Older DNOE01/DNOE02 records remain readable. When an older connection has no
locator, its disconnect remains an ordinary DNOE01 record with absent replay
context. Absence cannot justify inventing a view, skipping delivery or claiming
readiness. A present malformed/mismatched locator fails before canonical commit.

The reader checks checkpoint identity, parent continuity, frontier size/root,
pool bounds, canonical undo encoding and ordered MTP heights, in addition to
existing record/body/domain/sequence checks. Record and page budgets include the
replay payload; a too-small budget fails, rather than reporting end of history.
The 16 MiB record/page ceiling remains operational, not a consensus or resident
memory bound. Extra head/predecessor reads still occur outside the returned-page
budget. Retention is unpruned and load/storage growth remains unqualified.

This supplies source material for an immutable replay view. It does not construct
that view, certify pre-origin history/unspentness, replace proof/script validation,
install account recovery or a notification provider, or establish all-consumer
readiness. A future owner must check the retained material against actual bodies,
revalidate authorizations and reconstruct branch anchor/nullifier membership.
Local checksums do not authenticate an adversarial database. MTP answers are
retained validation inputs, not independent header-history certification.

The generated indexed regression checks exact persisted frame contents after
reopen and removal of active undo, page-budget enforcement, old-format absence,
re-sealed checkpoint mismatch refusal, corrupt locator refusal before writes,
rollback and existing crash-boundary cases. These fixtures are not independently
validated historical consensus. Their MTP maps are empty (no timestamp-lock
queries); nonempty-MTP delayed replay remains a later integration obligation.

## Local qualification

Fresh declared CMake builds of the full daemon and staging/service/index targets
passed. Eight distinct CTests passed after the final test update: runtime outbox,
indexed commit (including its existing process crash boundaries), commit owner,
service delivery source/startup/undo coverage/disconnect, and index delivery.
The runtime-reader-off service translation unit compiled. Both unchanged workflow
selectors still select exactly 46 enabled root registrations; not all 46 were
run locally.

All 69 linked project C++ translation units of the outbox executable were freshly
ASan/UBSan instrumented; the strengthened test source was rebuilt within the same
run. Dependency-first linking was retained from the prior qualified scope, and
final maps contain no project C++ archive members. Rust/external libraries were
uninstrumented and macOS leak detection was off. Four copied-source omissions
(capture, checkpoint identity validation, payload budget charge, and locator
validation) failed intended assertions; the restored executable passed. The
locator-control verifier initially expected a variable name, while the shared
fixture reports source line 33; its expected diagnostic was corrected without
changing production or test checks.

No original-source test-first, independent historical validation, whole-node
lifecycle, physical power-loss or release-binary provenance is claimed. Version
labels are inherited from 781896 and OpenSSL is prebuilt. The independent ARM
RocksDB alignment gate remains open.
