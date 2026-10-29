# Canonical delivery source selection

## Production behavior

The existing outbox reader now requires the retained head's post-transition
hash and height to equal the persisted canonical tip. This includes reads from
an already-applied head cursor that return no events. Indexed append and
historical preparation also refuse a retained head from another canonical tip;
indexed append additionally checks its new event's pre-transition tip.

Pages check adjacent transition identities as well as sequence and digest links:
connect moves parent/height-minus-one to block/height; disconnect reverses that
move. Pagination checks the first returned event against the supplied cursor's
record. No format, origin, deletion or acknowledgment semantics change. Existing
128-event/16-MiB limits remain returned-page operational limits. Head and cursor
record reads are additional; this is not a resident-memory bound.

`ChainstateService::getRuntimeDeliveryPage` owns the actual activation lock,
selects the configured network/profile, requires stateful service memory,
active tip, persisted tip and validated-tip identities to agree, and returns an
immutable copied page. Unsupported builds, CSN, invalid/inactive configuration
and inconsistent state refuse. Callers acquire the source before wallet leases;
a copied page cannot establish that the chain has not advanced afterwards.

The actual `VerifyConsensusJournalAtActiveTip` probes both the retained delivery
head and first record before its old historical/Orchard branches. Retained
history requires a successful bounded source check, including after rollback
below activation. A missing head cannot hide the first record. Stores with no
origin keep their previous startup behavior. Source refusal enters the existing
safe-mode path. The separate mandatory Orchard state audit remains in place.

## Qualification and limits

The original reader fails the new EOF/canonical-tip regression. The corrected
reader passes mixed connect/disconnect/reconnect replay, exact historical body
framing, and independently reframed records whose checksums and digest links
are consistent but whose adjacent transitions disagree. The old mixed-source
fixture jumped to an unrelated historical tip; it now uses an actual historical
body matching the Orchard parent header. This is identity framing, not validated
historical scripts, PoW or UTXO provenance.

An independent `OrchardServiceDeliverySource` CTest exercises the real service
API and startup gate over an indexed generated store, pagination, EOF, bad
cursor/bounds, memory disagreement, unsupported mode/profile, missing head,
unchanged logical rows and real indexed boundary rollback. The test restores
parent memory before checking the historical startup path. It is not a running
node or consumer integration test. Both workflow selectors require the lane.

The local declared CMake daemon build and eleven selected CTests pass; the
strengthened boundary lane is separately rerun after its final fixture change.
The default service translation unit compiles with optional runtime support off.
The outbox executable's 69 project C++ translation units were freshly built
with ASan/UBSan and pass the outbox lane plus head-tip/adjacency omission
controls. The final map contains no project C++ archive members. The test uses
the existing dependency-first link order; no sanitizer checks are suppressed.
Its external libraries and Rust are outside that project scope;
macOS leak detection is off. A separate attempt to instrument the complete
RocksDB dependency did not qualify on ARM. This remains an open platform gate,
with details and raw diagnostics retained in the private qualification ledger.
The actual service startup omission control uses a normal build, not a service
sanitizer claim.
Local inherited version labels and prebuilt dependencies are not release-binary
provenance. Exact Linux qualification is recorded separately by source commit.

Startup reads a bounded origin page and the head, not every middle event. A
consumer must validate every page from its actual applied checkpoint. Local
checksums are consistency checks, not adversarial authentication or consensus
certificates. The first event is coverage origin, not proof of earlier delivery.

## Remaining recovery work

No production notification provider or per-store completion is introduced.
Existing outbox, whole-reorg intent and encrypted account receipt remain the
recovery inputs. Ordinary wallet/index/note/account effects still need durable
per-store progress, ordered partial-commit recovery, historical and late-account
reconciliation and all-consumer readiness. Wallet source selection must not
advance a receipt merely because this API returned a page. Independently
validated activation history and full startup/replay/reindex qualification
remain open; mainnet activation is unset.
