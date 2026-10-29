# Orchard nullifier conflicts in prepared pool updates

An immutable Orchard pool body now owns all of its action nullifiers. Historical
bodies expose an empty Orchard list. These are structural identities; they do
not establish proof validity or authorize admission. Legacy shielded nullifiers
remain a separate domain.

The typed block-effect extractor retains every Orchard action nullifier,
including actions with disabled spends, matching the state transition's rule.
The caller must validate the exact block before using these effects.

The existing prepared pool update compares those identities with present
entries and collects each conflicting transaction plus its transparent
descendants before removal changes dependency indexes. A confirmed transaction
is removed as confirmed and keeps its eligible children. All changes remain
inside the existing rollback/preparation owner: abandoning preparation retains
the prior pool, and publication after canonical commit uses its existing
nonthrowing swap. No new admission index or persistence format is introduced.

The fixtures use an explicitly installed structural mixed-family graph and real
envelope decoding, block-effect extraction, pool indexes and output overlay.
They check retained bytes, all action identities, duplicate block identities,
abandon/publication, confirmation with a surviving child, and preparation
failure followed by retry. They do not claim Orchard authorization or real
admission/mining. Orchard submission remains unavailable; production block
notifications, all-consumer completion, readmission, shield/send/unshield and
release qualification remain open. Mainnet activation remains unset.
