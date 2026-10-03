# Orchard account delivery checkpoint

Date: 2026-09-26
Status: account-side implementation; production recovery ownership remains open.

## State and receipt move together

`OrchardAccountState::AdvanceDelivery` performs the existing note/witness scan
and pending-operation observation before attaching the next outbox receipt.
`RewindDelivery` restores the authenticated immediate parent scan, removes
orphaned observations, preserves current issued addresses and signed/pending
operations, and records the disconnect receipt. There is no cursor-only setter.
Untracked `Advance`/`RewindScanFrom` refuse an account that has a delivery receipt.

The methods require contiguous sequence and predecessor digest, a nonzero new
digest, matching selected domain/activation, direction, height/parent/current
scan, and exact candidate bytes including the suffix. Connect uses the existing
prepared-state/authorization checks. Rollback still requires the caller to
provide an authenticated retained parent; matching a hash alone does not
establish note or witness correctness.

`DNORAC04` appends a fixed 40-byte receipt to the encrypted account payload.
Versions 01/02/03 remain readable with no fabricated acknowledgment. The existing
`WalletSnapshotStore::StageReplace` therefore commits the receipt, scan,
observations, pending bytes and issued-address counters in one SQLite row and
outer transaction. It does not publish memory or commit for its caller.
Archive and other account updates preserve the receipt. A rescan deliberately
clears it along with derived scan/observations; it preserves addresses and signed
operations. Rescan is not synchronized readiness or permission to skip delivery.

## Ownership requirements still to implement

An event is a POD source record, not a sealed validity certificate. The owner
must obtain it from `ReadRuntimeOutboxUnderLock` against the selected profile,
verify the restored cursor against that source, hold the proper chain/wallet
locks, and obtain the prepared state, authorizations and retained parent from
qualified history. These methods do not independently recalculate the record
checksum or certify consensus validity.

This checkpoint covers one Orchard account only. It is not acknowledgment of
transparent wallet, mempool, relay or other consumers. The owner must persist
other wallet effects in the same outer transaction or retain a separate durable
handoff; in-memory queueing is insufficient. Only after commit may it publish
account state or report delivery. Duplicate events are rejected; recovery resumes
from the committed cursor rather than applying one event twice.

Historical events below activation are refused, not silently acknowledged.
Cross-boundary wallet handling, late-account initialization, complete rescan
reconciliation, archive backlog and production notification readiness remain
open. No permissive provider is installed and no activation is enabled.

## Qualification

The account and archive CTests pass against freshly compiled changed/dependent
objects and read-only cached libraries. Account tests use a real received note,
ready transaction, connect/disconnect/reconnect to the same tip, malformed event
metadata/bytes, old payload formats, encrypted rollback/reopen, and fresh child
process exits immediately before/after SQLite commit. The two process outcomes
assert the matching receipt, balance, observations, address counter and exact
pending transaction. Synthetic source digests test account binding; they do not
claim source-log or full-node qualification.

All 29 linked project C++ translation units in the account executable were
freshly instrumented with ASan/UBSan. Rust and external libraries remain
uninstrumented; macOS leak detection is disabled. Four copied-source controls
remove receipt publication, source ordering, body matching or rescan receipt
reset. These are account tests, not running-node lifecycle or release-binary
provenance. Exact-source Linux qualification is recorded separately.
