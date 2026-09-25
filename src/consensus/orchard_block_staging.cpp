#include "consensus/orchard_block_staging.h"
#include "storage/chain_db.h"

namespace dinero::consensus {
PreparedOrchardState StageOrchardBlockUnderChainstateLock(ChainDB& db,
    const ChainWriteToken& token, const OrchardBlockContext& context,
    std::span<const VerifiedOrchardAuthorizations> transactions, rocksdb::WriteBatch& batch) {
    const auto tip = db.getTip();
    if (!tip.ok()) throw OrchardStateLookupError(tip.status());
    if (context.height == 0 || tip->height < 0 ||
        static_cast<uint32_t>(tip->height) != context.height - 1 || tip->hash != context.parent_hash)
        throw OrchardStateError(OrchardStateErrorCode::Context);
    const auto stored = db.getOrchardState();
    std::optional<storage::OrchardStoredState> parent;
    if (stored.ok()) parent = stored.value();
    else if (stored.status() != Status::NotFound) throw OrchardStateLookupError(stored.status());
    if (parent) {
        // The committed tip itself must be represented in anchor history.
        // A missing local row is not an unknown anchor supplied by a peer.
        const auto refs = db.getOrchardAnchorReferences(parent->anchor);
        if (!refs.ok()) throw OrchardStateLookupError(
            refs.status() == Status::NotFound ? Status::Corruption : refs.status());
    }
    OrchardStateLookups lookups{
        [&](const uint256& root) -> StatusOr<bool> {
            const auto references = db.getOrchardAnchorReferences(root);
            if (references.status() == Status::NotFound) return false;
            if (!references.ok()) return references.status();
            return references.value() > 0;
        },
        [&](const uint256& nullifier) -> StatusOr<bool> {
            const auto owner = db.getOrchardNullifierOwner(nullifier);
            if (owner.status() == Status::NotFound) return false;
            if (!owner.ok()) return owner.status();
            return true;
        }};
    const auto prepared = [&] {
        try {
            return PrepareOrchardStateTransition(context, parent, transactions, lookups);
        } catch (const OrchardStateError& e) {
            // These bytes came from our committed database. Quarantine/recover
            // local state rather than marking the candidate block invalid.
            if (e.Code() == OrchardStateErrorCode::ParentState)
                throw OrchardStateLookupError(Status::Corruption);
            throw;
        }
    }();
    const auto staged = db.stageOrchardConnect(token, prepared.Parent(), prepared.Next(),
        prepared.Nullifiers(), prepared.Flows(), batch);
    if (staged != Status::Ok) throw OrchardStateLookupError(staged);
    return prepared;
}
} // namespace dinero::consensus
