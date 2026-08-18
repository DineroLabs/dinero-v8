#include "storage/forest_restore.h"

#include <cstring>

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_canonical_roots_activation.h"
#include "consensus/utreexo_delta.h"
#include "consensus/utreexo_delta_codec.h"
#include "storage/chain_db.h"

namespace dinero {
namespace storage {

Status ReplayUtreexoDeltaRange(const ChainDB& db,
                               consensus::UtreexoForest& forest,
                               uint32_t from_exclusive,
                               uint32_t to_inclusive,
                               std::string& error,
                               const BlockHashAtHeightResolver& resolve_hash) {
    for (uint32_t h = from_exclusive + 1; h <= to_inclusive; ++h) {
        // #579: identity first. The persisted height index can remain stale
        // across reorgs; a replay that trusts it pulls the rewritten branch's
        // hashes and fails ("replay-missing-header-at-N"), which permanently
        // wedged CSN reorg recovery (the speculative reorg-plan restore).
        uint256 block_hash;
        if (resolve_hash) {
            if (!resolve_hash(h, block_hash)) {
                error = "replay-anchor-walk-missing-hash-at-" + std::to_string(h);
                return Status::NotFound;
            }
        } else {
            auto hash_result = db.getBlockHashByHeight(static_cast<int>(h));
            if (hash_result.status() != Status::Ok) {
                error = "replay-missing-height-index-at-" + std::to_string(h);
                return Status::NotFound;
            }
            block_hash = hash_result.value();
        }

        std::string delta_blob;
        const auto blob_status =
            db.getRaw(MakeUtreexoDeltaUndoKey(block_hash), delta_blob);
        if (blob_status != Status::Ok) {
            error = "replay-missing-delta-sidecar-at-" + std::to_string(h);
            return Status::NotFound;
        }

        consensus::UtreexoDelta delta;
        std::string codec_error;
        if (!DeserializeUtreexoDelta(delta_blob, delta, codec_error)) {
            error = "replay-corrupt-delta-sidecar-at-" + std::to_string(h) +
                    ": " + codec_error;
            return Status::Invalid;
        }

        // Mirror ConnectBlockInternal's canonical-roots fork activation:
        // flip + re-canonicalize before applying the activation block.
        if (consensus::IsUtreexoCanonicalRootsActive(h) &&
            !forest.isCanonicalEmptyRoots()) {
            forest.setCanonicalEmptyRoots(true);
            forest.rebuildRoots();
        }

        std::string apply_error;
        if (!ApplyUtreexoDeltaForward(forest, delta, apply_error)) {
            error = "replay-apply-failed-at-" + std::to_string(h) + ": " +
                    apply_error;
            return Status::Invalid;
        }

        // Every replayed block must land exactly on the header commitment —
        // the same check reindex enforces. A wrong or reordered sidecar
        // fails loudly here instead of surfacing blocks later.
        auto header_result = db.getHeader(block_hash);
        if (header_result.status() != Status::Ok) {
            error = "replay-missing-header-at-" + std::to_string(h);
            return Status::NotFound;
        }
        const consensus::UtreexoHash commitment = forest.getCommitment();
        uint256 computed_root;
        if (commitment.size() == 32) {
            std::memcpy(computed_root.data, commitment.data(), 32);
        }
        if (computed_root != header_result.value().utreexo_root) {
            error = "replay-root-mismatch-at-" + std::to_string(h);
            return Status::Invalid;
        }
    }
    return Status::Ok;
}

Status RestoreHistoricalForest(const ChainDB& db, uint32_t target_height,
                               consensus::UtreexoForest& out,
                               std::string& error,
                               const BlockHashAtHeightResolver& resolve_hash) {
    auto checkpoint_result =
        db.getLatestUtreexoCheckpointAtOrBelow(static_cast<int>(target_height));
    if (checkpoint_result.status() != Status::Ok) {
        error = "restore-no-checkpoint-at-or-below-" +
                std::to_string(target_height);
        return checkpoint_result.status();
    }
    const uint32_t checkpoint_height =
        static_cast<uint32_t>(checkpoint_result.value().first);
    const std::vector<uint8_t>& checkpoint_blob = checkpoint_result.value().second;

    consensus::UtreexoForest restored =
        consensus::UtreexoForest::deserialize(checkpoint_blob);
    // deserialize is all-or-nothing (rooted-husk fix): a refused payload
    // comes back EMPTY. Treat empty-for-non-empty as corruption.
    if (restored.getNumLeaves() == 0 && !checkpoint_blob.empty() &&
        checkpoint_height > 0) {
        error = "restore-checkpoint-deserialize-refused-at-" +
                std::to_string(checkpoint_height);
        return Status::Invalid;
    }
    if (consensus::IsUtreexoCanonicalRootsActive(checkpoint_height)) {
        // Full-node checkpoints are serialized with the canonical flag ON,
        // so their roots_ are already canonical and just need the flag set.
        // CSN/stateless checkpoints, however, are serialized from a forest
        // that never called setCanonicalEmptyRoots (see stateless_node.cpp)
        // — flag stored OFF, roots_ NOT canonical. Force-setting the flag
        // on those without recomputing yields a non-canonical root and a
        // spurious header mismatch (observed at CSN reorg fork restore).
        // So: if the stored flag was OFF at a post-activation height,
        // recanonicalize roots_ from nodes_ via rebuildRoots(); if it was
        // already ON, leave it (rebuild would reproduce the same values).
        const bool stored_canonical = restored.isCanonicalEmptyRoots();
        restored.setCanonicalEmptyRoots(true);
        if (!stored_canonical) {
            restored.rebuildRoots();
        }
    }

    // Verify the checkpoint itself against its own height's header root.
    // An exact-checkpoint-hit restore replays nothing, so without this a
    // corrupt-but-parseable checkpoint would restore silently. (Genesis is
    // exempt: an empty forest is its own evidence, and any replay from it
    // verifies block 1+ against headers immediately.)
    if (checkpoint_height > 0) {
        // #579: same identity-first rule as the replay loop — the checkpoint
        // self-check must verify against the header of the block the RESTORED
        // CHAIN holds at the checkpoint height, not whatever a possibly-stale
        // height index answers.
        uint256 ckpt_hash;
        if (resolve_hash) {
            if (!resolve_hash(checkpoint_height, ckpt_hash)) {
                error = "restore-anchor-walk-missing-hash-at-checkpoint-" +
                        std::to_string(checkpoint_height);
                return Status::NotFound;
            }
        } else {
            auto ckpt_hash_result =
                db.getBlockHashByHeight(static_cast<int>(checkpoint_height));
            if (ckpt_hash_result.status() != Status::Ok) {
                error = "restore-missing-height-index-at-checkpoint-" +
                        std::to_string(checkpoint_height);
                return Status::NotFound;
            }
            ckpt_hash = ckpt_hash_result.value();
        }
        auto ckpt_header_result = db.getHeader(ckpt_hash);
        if (ckpt_header_result.status() != Status::Ok) {
            error = "restore-missing-header-at-checkpoint-" +
                    std::to_string(checkpoint_height);
            return Status::NotFound;
        }
        const consensus::UtreexoHash ckpt_commitment = restored.getCommitment();
        uint256 ckpt_root;
        if (ckpt_commitment.size() == 32) {
            std::memcpy(ckpt_root.data, ckpt_commitment.data(), 32);
        }
        if (ckpt_root != ckpt_header_result.value().utreexo_root) {
            error = "restore-checkpoint-root-mismatch-at-" +
                    std::to_string(checkpoint_height);
            return Status::Invalid;
        }
    }

    const Status replay_status = ReplayUtreexoDeltaRange(
        db, restored, checkpoint_height, target_height, error, resolve_hash);
    if (replay_status != Status::Ok) {
        return replay_status;
    }

    out = std::move(restored);
    return Status::Ok;
}

}  // namespace storage
}  // namespace dinero
