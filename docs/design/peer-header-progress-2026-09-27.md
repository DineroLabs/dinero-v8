# Exact peer header progress

## Problem and change

The completed Linux full run at bf638 reported a peer height of 13 after the
receiving node had converged onto that peer's branch at height 14. The run's
private receipt preserves the failure. It does not establish the exact
interleaving or attribute the failure to the preceding timeout change.

The daemon previously updated header telemetry only for newly inserted header
batches and used the selector's global best height. Block paths updated peer
block progress after body ingestion, while scheduler ingestion loses the
sending peer address. A winning body can therefore be visible before that
caller's peer update, or another owner can accept it first.

`HeaderSyncManager::ProcessResult` now supplies the maximum exact height of a
fully accepted nonempty batch, including duplicate-only batches. Every height
comes from a copied entry for that batch's header hash. Rejected batches and
failed exact lookups do not supply an estimate. The daemon uses this result for
`synced_headers`; stale-tip clock behavior remains based on new insertions.

For incoming full and compact blocks, the daemon observes the header before
passing the body to the scheduler or relay. `ObserveBlockHeader` uses
`ValidateObservedHeaderHeight` under the selector lock. Known headers return
their own height. New headers require a known parent, the existing header
validation rules, and a non-overflowing parent height. Unrelated genesis
identities refuse. This read does not insert a header, change the best-work tip,
consume a headers request, or alter body routing. No selector lock is held while
publishing peer telemetry or calling body handlers.

The daemon updates only the actual sending peer's header knowledge. It does
not advance `synced_blocks` on a header observation or substitute the local tip.
Existing body-validation, activation, stateless routing, and stale-clock gates
remain in force. Failed header observations leave body routing to its existing
owner; they do not introduce an alternate acceptance path.

## Qualification scope

Three new `PeerHeaderProgress` cases use the real selector and header-sync
adapter under the existing regtest profile. They check lower side-branch
heights despite a higher local tip, duplicate and empty batches, refused late
rows, header observation without tip/request mutation, and invalid/missing
parent refusal. The regtest profile skips proof of work; these cases are not a
mainnet proof-of-work qualification. Existing header integration case bodies
and the five-variant `MinorityTipHeaderRelay` script are unchanged.

Final fresh backend ON and OFF builds completed the full daemon and declared
header-sync target. Each configuration passed three CTests: the new component
cases, all 11 original header-sync integration cases, and all five unchanged
real two-node daemon variants (15 hard assertions). An earlier ON pass preceded
the final reconstructed/orphan callback addition and is retained separately;
the final ON build used another fresh directory.

All 19 project C++ files in the component's actual link map were freshly built
with ASan/UBSan; all three new cases passed and 1,240 source/header input hashes
remained stable. Three copied omissions (skip duplicate height, use global best
height, insert an observed header) failed the intended assertions without
sanitizer diagnostics. Restored code passed. The instrumented link maps contain
no project C++ archive members. External libraries, the daemon, and OFF binaries
are outside this component sanitizer scope; macOS leak detection was disabled.
The separate completed full-RocksDB ARM qualification remains independently scoped.

Compiler pools were sequential, with at most four local workers. No initial
original-source red run is claimed. The original daemon fixture and its deadlines
remain unchanged. A fresh Linux full run must still verify the originally failing
lane; these local passes do not prove the precise historical CI interleaving.

This change does not supply a global readiness acknowledgment, bind telemetry
to a persistent peer session across reconnect, or qualify Orchard release,
platform/load behavior, pending ownership, or mainnet activation.
