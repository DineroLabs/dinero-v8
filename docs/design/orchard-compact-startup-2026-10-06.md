# Independent compact startup reconstruction

Status: implemented component, qualification pending. Production service startup and CSN routing remain guarded; this is not release readiness.

`OrchardReindexOwner::RestoreCompact` creates a new compact owner only after independently reconstructing the retained history. It requires exclusive startup/datadir ownership and holds the actual future writer mutex throughout. The returned owner binds the actual source ChainDB and mutex; neither a temporary candidate pointer, caller-supplied catalog, nor successful reindex statistics can enroll it.

The full `Run` entry retains its storage-mode refusal. A private shared reconstruction path independently validates historical genesis-to-parent bodies, retirement, complete catalog membership and retained canonical connect/disconnect chronology. Compact startup copies archival block files into a newly created scratch directory. Every writable reindex destination, block/undo output and shielded sidecar belongs to that directory. Original source paths and exact source locators remain distinct. The method neither copies rebuilt coins into the source nor repairs missing rows.

Every replayed connected catalog and predecessor must match the original exactly, including every reachable immutable node. Source archival bodies and undo must match reconstruction. Final Orchard state, logical commitment sets, retirement, accumulator marker, validated tip, work and storage profile must agree. A reverse audit checks selected journals, bodies, transaction indexes, catalog predecessors and undo through the activation boundary. A staged but abandoned compact disconnect checks current logical owners and frozen legacy state. At the boundary parent, the replay-derived retirement owner is authenticated against the original legacy state and retained for reconnect.

Before enrollment, the held startup owner rechecks source tip, validated tip, delivery head, selected catalog, mode and original block-file inventory/digests. No callback or unlocked handoff exists. This requires all writers to obey the supplied mutex and exclusive startup ownership; it is not multiprocess or arbitrary reentrant mutation protection.

A final tip below activation minus one still requires an explicit historical storage-mode reconstruction transition. It cannot be represented by the current compact catalog State and is refused. Actual DaemonApp promotion/reopen integration, service admission/mining/notifications and all configured CSN consumers remain required. Scratch reconstruction uses full replay storage; this is not bounded-resident-memory or general history/load qualification.

Three new component cases use genuine archived historical blocks, real proof of work, actual full and compact canonical writers, a shield and modern same-block parent/child spend. They destroy the original live owner and reopen a different ChainDB object. Tests require unchanged source logical rows and original block/undo file bytes during authentication, then real compact undo and reconnect with a freshly authenticated boundary owner. Refusal cases cover a well-formed forged catalog, a missing reachable historical transaction node, wrong archive-reader domain and damaged selected journal. No production process startup, power loss, platform or release claim is made.

The API and new tests were introduced together: no initial original-source RED claim. Existing tests and deadlines are unchanged. All qualification results remain pending until actual execution and independent private preservation. Mainnet height and branch remain unset.

Initial v1 syntax checks passed the production reconstruction and writer but rejected the new fixture's pointer type: SolveCatalogTemplate returns a CatalogSolvedTemplate wrapper, not an OrchardMiningTemplate. Exact source and diagnostics were privately preserved before the fixture-only correction. Three pointer declarations now use the actual wrapper type; all fixture operations/assertions and production source remain unchanged. No runtime tests ran in v1. Fresh v2 qualification is pending.

Fresh v2 built the ON daemon and replay target; 11 selected CTests ran (10 passed, one failed), with 69 of 70 internal cases passing. The fresh-owner lifecycle case threw during reconstruction after boundary undo. OFF, sanitizer and copied controls did not start. Exact source and terminal diagnostics were privately preserved. Source review identified that historical replay compared Orchard undo records against the current source even for blocks subsequently disconnected; the real disconnect deletes those records. V3 moves that comparison to the final selected reverse audit, comparing every selected predecessor against the independently replayed candidate. It keeps catalog/node and conventional archival undo comparisons across the retained history. All fixture operations, assertions and deadlines are unchanged; fresh qualification is required before calling this repaired.

## Complete current Orchard storage comparison

Startup compares every key and value in the original O1 namespace with the independently reconstructed candidate before creating the owner. Ordered RocksDB iterators use constant working memory beyond their own buffers and check both read statuses before treating either range as complete. Unknown or orphan records, a missing suffix, and nonempty records at the activation parent refuse; no source repair is performed. This local equality check does not authenticate an arbitrary expected store. Independent replay and the existing exclusive startup/writer ownership remain mandatory.

Two additional cases cover actual extra undo/unknown records at an active tip, orphan nullifier/anchor/undo/unknown records at the boundary parent, unchanged source/archive bytes on refusal, exact record removal and successful fresh restore/reconnect. The storage comparator also uses forwarding wrappers around real RocksDB iterators to inject initial, intermediate and terminal read errors on either side, then checks healthy retry. This tests error propagation, not physical I/O failure or concurrent database mutation. Closed owners refuse. Existing tests, deadlines, selected undo/state/commitment/retirement/catalog and archival comparisons remain unchanged.

This additional comparison and its tests require fresh qualification. Previous v3 qualification, when preserved, applies only to the earlier source. Production Init/CSN/history acquisition/reindex promotion and whole-node/platform/release requirements remain open.

## Service integration in progress

`ChainstateService::Init` now selects a separate compact-storage route from the
checked durable binding. That route calls the actual independent
`RestoreCompact` reconstruction with the service's activation mutex and database,
then retains the resulting owner in the service. It creates no full coin map,
forest, legacy shielded sidecar, wallet index or global proof-position index.
Selected-read and wallet admission remain closed. The owner is destroyed before
its bound mutex/database; database setters refuse replacement while it exists.
Existing `orchard-startup-replay` scratch is refused, never adopted or deleted.

This is unfinished source, not a qualified production startup path. `Start`
explicitly refuses the compact route while canonical connection, selected-tip
publication, pool proof validation and configured consumers are being connected.
DaemonApp also refuses a compact binding immediately after opening ChainDB,
before its offline undo repair, genesis and header/height backfills. This outer
hold must be replaced by the complete mode-specific composition, not simply
removed. The existing promotion-journal recovery before opening ChainDB is a
separate lifecycle that still needs review. The daemon entry point's datadir
guard spans startup and service lifetime; callers of isolated service APIs must
provide the same exclusive startup ownership.

The service edits have passed fresh ON/OFF daemon builds and focused normal
component checks. Those checks exercise initialization refusal and the compact
inverse writer; they do not qualify complete service startup or every detached
capture/completion/binding path. Full startup, admission/readmission,
canonical connect/disconnect/reorg, all consumers and whole-node qualification
remain required before opening this route.

The selected pool context now accepts an explicit optional body/proof inventory.
Compact admission supplies pending plus incoming entries; selection supplies its
entire candidate list. The compact owner authenticates every captured input and
output absence, and proof checks use its selected stump. General callers without
an inventory retain the full-node view and refuse compact storage. Authorization,
resource, nullifier, fee and reservation-capacity checks remain in the actual pool
validator. This does not yet provide compact wallet selection or mining, and the
existing transport still cannot prove pending-parent inputs.

Selected-tip and delivery-source audits now distinguish the enrolled compact
owner from full memory coins. The tip audit prepares and abandons the actual
compact disconnect, checking inverse state/catalog effects and retained
body/undo/outbox records without committing. Delivery-page reads authenticate the
compact selection before reading the existing outbox. The integration paths compile in both configurations; startup admission remains
closed pending the complete runtime composition and its qualification.

Compact connect staging now accepts the existing sealed `ValidatedOrchardBlock`
result. Both full and compact adapters run the same exact-wire, profile, witness,
parent header/root, Orchard state/membership, external input, created-output
absence and recorded MTP comparisons. Compact before-images come from the
authenticated catalog/proof view; full before-images still come from the full
coin database. Only after those checks may staging reuse the sealed coin and
authorization results. Catalog/retirement, state commitment, transition, filter,
undo, journal and outbox checks still run before the canonical commit.

The service connector selects that compact writer only with an enrolled owner
and a completed detached result. It retains consumer/pool preparation and the
existing post-commit publication order. Detached compact extension capture,
completion and binding now use a captured catalog/stump instead of a full forest.
Capture authenticates the candidate inputs and output absences and records their
required MTP answers. Completion verifies authorizations and the canonical stump
transition outside the selected lock. Binding recaptures inputs, compares the
selected catalog and independently derived retirement owner, checks the resulting
state commitment and runs the shared sealed-parent comparison before publishing
the immutable validation result. The retirement accessor exposes only the record
of an already enrolled owner under its bound mutex; it creates no owner or key.

The extension path has been compiled in the full daemon builds; its compact
service lifecycle still requires direct qualification. Compact inverse staging now also
accepts the existing private validation seal. It compares the selected child
state, retained parent state and membership, retirement, exact wire, witness
policy, parent root, input before-images, output absences and recorded MTP before
reusing authorization. The canonical proof transition, undo/delta/catalog,
filter, journal and outbox checks still run. The service audit and disconnect
require a matching completed branch result from their pinned preparation scope;
they refuse when it is absent. Full-node forest locking and downstream
prepare/commit/publication ordering are retained. A compact-native detached
inverse capture and startup/activation composition are still required; this
change does not create that owner or open startup.
At the activation parent the service explicitly refuses the legacy in-memory
journal path, which has no valid compact memory state; the real boundary audit
remains to be connected. The inverse production code passed fresh enabled- and disabled-backend daemon builds.
The two inverse fixtures use the independently verified block's actual witness
policy and invert it explicitly for the refusal case. They run as the separate,
mandatory `OrchardCompactInverse` suite with a 120-second limit. The existing
catalog suite and its 180-second limit remain unchanged. Every fixture and
assertion body is retained; only the two new cases' suite names change. This
registration change requires fresh qualification. The cases use genuine independent historical and Orchard replay
to obtain seals, then exercise abandonment, both disconnects across the boundary,
reopen/reconnect and rejection of a foreign seal or changed witness policy.
No complete compact-runtime or original-source failure qualification is claimed.
