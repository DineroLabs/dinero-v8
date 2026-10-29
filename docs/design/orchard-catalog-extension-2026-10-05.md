# Catalog-backed extension input capture

The real detached extension service captures a completed parent catalog under
selected-parent ownership. At the first boundary it uses the independently
prepared history owner; later blocks require the canonical parent's existing
record. Domain, height, header, work and verification stump must agree with the
selected full node. Binding the completed extension rechecks the same encoded
catalog before canonical writes.

Detached completion resolves candidate metadata using that captured catalog.
Complete transaction membership establishes output absence, including IDs whose
outputs were entirely spent. Legacy metadata is queried independently of the
peer's claimed height. Modern creation metadata must pass the exact accumulator
proof. Nontransparent membership refuses stripped confidential metadata at any
height. Missing referenced nodes are errors. Same-block inputs remain subject
to ordered resolution and exact metadata comparison.

The resulting immutable candidate view feeds the actual mixed-transaction
validator. Its capture does not consult a full coin database, full forest or
optional transaction index. The service retains its existing independently
captured full-node coin comparisons, full-forest transition and canonical
before-image checks during this integration stage.

This is an intermediate production integration, not a root-only node release.
The service still requires its full node and refuses stateless operation.
Root-only live/durable ownership, canonical connect/undo, startup/reindex and
configured compact consumers remain required. A decoded catalog alone never
authorizes a selected parent. No activation height is set.

The extension revision completed fresh full ON/OFF daemon builds and 17 selected
CTest executions: 57 actual cases with the backend ON and 15 OFF. All 323 linked
C++ translation units were freshly instrumented for ASan/UBSan; 57 cases passed.
Three copied validation omissions failed the intended assertions, and 20 restored
cases passed. Evidence is preserved privately. External dependencies remained
uninstrumented and macOS leak detection was off; these results do not qualify
root-only CSN, the whole node, or a release. Subsequent compact writer edits need
fresh qualification.

The fixtures use actual historical-boundary and
descendant proof data, including a same-block child. Typed in-memory catalog
mutations exercise adapter refusal only and are never installed as canonical
authority. Existing production checks, assertions and deadlines remain.
