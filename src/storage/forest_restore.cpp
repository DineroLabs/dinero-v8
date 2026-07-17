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
                               std::string& error) {
    for (uint32_t h = from_exclusive + 1; h <= to_inclusive; ++h) {
        auto hash_result = db.getBlockHashByHeight(static_cast<int>(h));
        if (hash_result.status() != Status::Ok) {
            error = "replay-missing-height-index-at-" + std::to_string(h);
            return Status::NotFound;
        }
        const uint256 block_hash = hash_result.value();

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
                               std::string& error) {
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
        // The checkpoint's roots_ are already canonical (saved by a
        // flag-on node); set the flag without rebuilding, exactly like
        // the startup restore does.
        restored.setCanonicalEmptyRoots(true);
    }

    // Verify the checkpoint itself against its own height's header root.
    // An exact-checkpoint-hit restore replays nothing, so without this a
    // corrupt-but-parseable checkpoint would restore silently. (Genesis is
    // exempt: an empty forest is its own evidence, and any replay from it
    // verifies block 1+ against headers immediately.)
    if (checkpoint_height > 0) {
        auto ckpt_hash_result =
            db.getBlockHashByHeight(static_cast<int>(checkpoint_height));
        if (ckpt_hash_result.status() != Status::Ok) {
            error = "restore-missing-height-index-at-checkpoint-" +
                    std::to_string(checkpoint_height);
            return Status::NotFound;
        }
        auto ckpt_header_result = db.getHeader(ckpt_hash_result.value());
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
        db, restored, checkpoint_height, target_height, error);
    if (replay_status != Status::Ok) {
        return replay_status;
    }

    out = std::move(restored);
    return Status::Ok;
}

}  // namespace storage
}  // namespace dinero
