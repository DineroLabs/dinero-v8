# Typed Orchard service connection

`ChainstateService::ConnectTip` now selects the Orchard route before the
historical validator and decoder. For a descendant of an already restored
Orchard tip, it holds the service activation mutex, checks the exact parent
pointer/identity, selected header ancestry and cumulative work, and invokes the
contextual header gate (time, network, checkpoints, ASERT and PoW under the
selected network policy). Sequence-lock time lookups are anchored to that same
parent branch. Missing local ancestry and future time are retryable; deterministic
header failures have an explicit consensus-invalid result.

The current parent's mandatory consistency audit must pass. Typed downstream
notifications must prepare without publishing or mutating canonical state.
The indexed owner then validates and stages the full descendant transition,
fsyncs body/undo material, and commits coin state, Orchard state, forest,
retirement markers, indexes, journal and locators in one synchronous batch.
After the service's contextual header validation, it requests validity flags
in that same batch; helper callers retain the default of no validity promotion.
The owner publishes coin/forest memory and index metadata after durability.
The service clears the diagnostic position cache, publishes the active tip and
then publishes the prepared event. Post-write failures cannot return normally
with divergent state. Caller-supplied raw batches or legacy transaction shells
are not used.

## Deliberate remaining boundaries

The first activation block still refuses with
`orchard-connect-boundary-history-unavailable`: persisted tip flags or a raw
retirement receipt are not certification of historical accounting. A service-owned
validated history/provenance source must be integrated before activation can
connect. This restriction does not reopen waived legacy recovery or holder work.

No production `RuntimeBlockNotifications` provider is installed. Actual daemon
connections therefore remain disabled pending complete wallet, mempool, relay,
proof-cache, oracle and long-poll consumer handling. Ordinary confidential
transaction compatibility, comprehensive body failure classification, complete
ActivateBestChain/replay/reindex/startup routing, CSN and pruned history remain
unfinished. The reorg fork-point reader now has selected typed routing; that
check alone does not qualify the whole reorg orchestration. No network activation
is configured.

## Qualification scope

The independent `OrchardServiceConnect` test invokes the actual service against
generated state with exact indexed flatfiles and a test-only notification
observer. It constructs regtest ancestry from canonical genesis, checks computed
chainwork, rolls a descendant back and reconnects it, then repeats rollback.
It covers missing header/consumer readiness, refused notification preparation,
wrong cumulative work, a conflicting configured checkpoint, validity persistence,
complete memory/disk agreement and publication ordering. Boundary connection
remains refused. The original service fails the new reconnect assertion.

The fixture uses ordinary regtest's explicit PoW bypass; the separate contextual
header suite covers actual work and ASERT. The synthetic retirement amount is
fixture data, not real-chain accounting. These service tests are not a running
node, production consumer qualification or final release-binary provenance.
