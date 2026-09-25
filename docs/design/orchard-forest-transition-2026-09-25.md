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
