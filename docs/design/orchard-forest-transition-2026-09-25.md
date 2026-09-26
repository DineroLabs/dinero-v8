# Stateful Orchard forest transition

The mixed coin preparation now retains the selected height and parent identity.
`PrepareOrchardForestTransition` requires a matching parent header and full forest
commitment, clones the forest once through the existing height-aware factory,
and applies the authorized mixed transaction effects to that private clone.

Ordering matches the existing full-node algorithm: remove external inputs, then
add surviving outputs in transaction/output order. A parent output spent inside
the same block never enters the forest. Leaves use the shared creation-height,
amount, script and coinbase hashing rules; no Orchard transaction is converted
to a historical transaction shell to obtain an identity.

The result carries its derived root, exact candidate identity and actual delta
positions. `MatchesHeader` requires both candidate identity and root. Template
construction must finalize the header before obtaining a result valid for that
final candidate; a result for a placeholder header cannot authorize another hash.

The rollback helper operates on another private clone, checks the selected after
root/count/canonical mode, reverses the delta, and requires the parent count/root.
Neither success nor failure mutates the supplied live forest. The delta uses
the existing UD sidecar codec and forward-replay implementation; coordinated
durable persistence and in-memory publication are not implemented here.

Tests compare the mixed transition against explicit sequential deletions and
the known surviving outputs, over four forest shapes. They exercise parent/root
rejection, missing membership, final-header matching, exclusion of transient
outputs, serialization/reload and exact internal-state restoration.
The actual UD serializer/decoder and forward replay reproduce the same after
forest, establishing compatibility with that existing recovery component.

This is a stateful computation component, not permission to skip peer-proof
checks. Utreexo payload format/targets/metadata authentication, resource/header
validation, atomic forest/tip/undo persistence and actual daemon/CSN callers
remain separate release requirements. No live parser or activation changes.

Native macOS qualification: the daemon builds and 18 selected tests pass
(13 Orchard lanes plus five existing Merkle/witness tests). The added persisted
delta forward-replay check also passes. The forest test passes ASan/UBSan with
all project C++ translation units selected by its link map instrumented; external
dependencies and Rust remain uninstrumented, and macOS leak detection is off.
Linux exact-head qualification remains separate.

## Stateful peer proof binding

The combined atomic adapter now requires the candidate's Utreexo payload. Its root-before and leaf count must match the authenticated full parent forest; the selected-height proof format must match. Every spent-output metadata row must equal the resolved coin in transaction/input order, including ephemeral parent/child inputs. The proof target set covers external inputs exactly once and excludes ephemeral inputs. Positions are unique and in range.

The path uses the existing stateless proof verifier with an explicit exact sibling-count check for the per-target path format emitted by the production `generateBlockProof` routine. It does not change historical verification's acceptance rules. Empty external-target proofs carry no hashes or positions. A syntactically canonical body with incorrect metadata, omitted/extra targets, duplicate positions, mismatched root/count/format, altered authentication paths or unused proof hashes fails the new path. Tests also accept regenerated proofs in a different target order.

The fixture initially used the deprecated deduplicated `generateBatchProof` helper, which is incompatible with sequential paths for larger forest shapes. It now uses the same `generateBlockProof` routine as mining. Forest padding 0,1,3,7 exercises different tree/path shapes. This is a stateful cross-check of peer metadata against resolved coins; an independent CSN coin-resolution/authorization path is still required.
