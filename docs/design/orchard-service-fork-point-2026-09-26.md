# Orchard fork-point forest verification

After disconnecting a branch, ActivateBestChain must verify the restored forest
before connecting the replacement branch. Its former check always decoded a
historical Block. A selected Orchard fork point has a mixed body, so the old
reader refused it even after a correct typed rollback.

The actual call now uses VerifyForkPointForestUnderLock under the existing
activation mutex. For an Orchard height, the fork must be the selected active tip,
live coin state and durable/validated/forest-marker tips must agree, and the
indexed availability/locator metadata must match. The typed reader authenticates
exact stored body identity, framing, transaction root and required witness
commitment. The forest marker and current live forest must match that body's
Utreexo root. Missing/failed metadata, unsupported runtime/CSN, wrong selected tip
or inconsistent roots return false; the caller aborts before its new connect loop.
No database batch, live state or validity status is modified by the check.

Historical heights keep the historical body reader and root comparison. An
Orchard height never falls back to a historical shell when runtime support or
retained material is unavailable. This check authenticates the restored fork's
body/root; it does not redo all historical scripts, Orchard proofs, ownership
metadata or wallet recovery.

The independent OrchardServiceForkPoint test invokes the exact method used by
ActivateBestChain after the real service's descendant rollback in a generated
store, then exercises the existing reconnect/rollback cycle. It checks accepted
mixed bodies, rejection of another selected state, wrong in-memory forest,
unavailable body metadata, restoration and no logical database changes. Both
checkpoint fixture modes run. The extracted old reader fails the first mixed-body
assertion; the typed implementation succeeds. This is service-fixture evidence,
not a complete running-node reorg or release qualification.

Activation-history provenance, production notification consumers, remaining
ActivateBestChain transaction reconciliation, startup/replay/reindex/CSN/pruning
and ordinary confidential transaction compatibility still need integration.
Mainnet activation stays unset.
