#include "storage/checkpoint_retention.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include "consensus/utreexo_accumulator.h"
#include "crypto/sha256.h"
#include "storage/chain_db.h"
#include "storage/forest_restore.h"

namespace dinero::storage {
namespace {
using Digest = std::array<uint8_t, 32>;

Digest HashBytes(const std::vector<uint8_t>& bytes) {
    Digest digest;
    crypto::CSHA256().Write(bytes.data(), bytes.size()).Finalize(digest.data());
    return digest;
}

struct AnchorIdentity {
    Digest bytes_hash;
    std::optional<std::vector<uint8_t>> checksum;
    bool operator==(const AnchorIdentity&) const = default;
};

Status ReadAnchor(const ChainDB& db, uint32_t height, AnchorIdentity& identity,
                  std::string& error) {
    const auto checkpoint = db.getUtreexoCheckpoint(static_cast<int>(height));
    if (!checkpoint.ok()) {
        error = "retention-missing-anchor-at-" + std::to_string(height);
        return checkpoint.status();
    }
    identity.bytes_hash = HashBytes(checkpoint.value());
    const auto checksum = db.getUtreexoChecksum(static_cast<int>(height));
    if (checksum.ok()) {
        identity.checksum = checksum.value();
        if (checksum.value().size() != identity.bytes_hash.size() ||
            !std::equal(checksum.value().begin(), checksum.value().end(),
                        identity.bytes_hash.begin())) {
            error = "retention-anchor-checksum-mismatch-at-" + std::to_string(height);
            return Status::Corruption;
        }
    } else if (checksum.status() == Status::NotFound) {
        // Legacy/CSN raw checkpoints legitimately have no checksum. Their
        // actual bytes and verified forest root remain mandatory bindings.
        identity.checksum.reset();
    } else {
        error = "retention-anchor-checksum-read-failed-at-" + std::to_string(height);
        return checksum.status();
    }
    return Status::Ok;
}
} // namespace

struct CheckpointRetentionPass::State {
    ChainDB& db;
    CheckpointRetentionPolicy policy;
    bool apply;
    CheckpointRetentionProgress progress;
    Status failed = Status::Ok;
    std::string failure;
    TipInfo tip;
    uint32_t first = 0, last = 0, probe = 0, delete_cursor = 0;
    int64_t walk_height = 0;
    uint256 walk_hash;
    std::vector<uint256> ancestry;
    AnchorIdentity left, right;
    std::optional<std::pair<uint256, uint32_t>> prebase;
    std::unique_ptr<consensus::UtreexoForest> forest;

    State(ChainDB& db_in, CheckpointRetentionPolicy policy_in, bool apply_in)
        : db(db_in), policy(std::move(policy_in)), apply(apply_in) {}

    Status fail(Status status, const std::string& reason, std::string& error) {
        failed = status;
        failure = error = reason;
        return status;
    }

    bool resolve(uint32_t height, uint256& hash) const {
        if (height < first || height > static_cast<uint32_t>(tip.height)) return false;
        hash = ancestry[height - first];
        return true;
    }

    Status stableTip(std::string& error) {
        const auto current = db.getTip();
        if (!current.ok()) return fail(current.status(), "retention-tip-read-failed", error);
        if (current.value().hash != tip.hash || current.value().height != tip.height) {
            return fail(Status::Invalid, "retention-tip-changed", error);
        }
        const auto validated = db.getValidatedTip();
        if (!validated.ok() || validated.value().hash != tip.hash || validated.value().height != tip.height) {
            return fail(Status::Invalid, "retention-storage-and-validated-tip-disagree", error);
        }
        return Status::Ok;
    }

    // Root-only replay is insufficient: a stale height index can splice
    // same-root headers from unrelated branches. Require the pinned ancestry
    // and its exact parent links as well as every replayed commitment.
    Status linkedHeight(uint32_t height, std::string& error) const {
        const auto hash = db.getBlockHashByHeight(static_cast<int>(height));
        if (!hash.ok() || hash.value() != ancestry[height - first]) {
            error = "retention-height-index-disagrees-at-" + std::to_string(height);
            return Status::Invalid;
        }
        const auto header = db.getHeader(hash.value());
        if (!header.ok()) {
            error = "retention-header-missing-at-" + std::to_string(height);
            return header.status();
        }
        if (height > first && header.value().prev_block_hash != ancestry[height - first - 1]) {
            error = "retention-header-link-mismatch-at-" + std::to_string(height);
            return Status::Invalid;
        }
        return Status::Ok;
    }

    void nextInterval() {
        progress.interval_start = progress.interval_end;
        forest.reset();
        progress.phase = CheckpointRetentionPhase::SelectInterval;
    }

    void skipInterval(const std::string& reason) {
        ++progress.skipped_intervals;
        progress.last_skip_reason = reason;
        nextInterval();
    }

    Status boundAnchors(std::string& error) const {
        AnchorIdentity current_left, current_right;
        auto status = ReadAnchor(db, progress.interval_start, current_left, error);
        if (status != Status::Ok) return status;
        status = ReadAnchor(db, progress.interval_end, current_right, error);
        if (status != Status::Ok) return status;
        if (current_left != left || current_right != right) {
            error = "retention-anchor-changed";
            return Status::Invalid;
        }
        status = linkedHeight(progress.interval_start, error);
        if (status != Status::Ok) return status;
        return linkedHeight(progress.interval_end, error);
    }

    Status step(const ChainWriteToken& token, std::string& error) {
        error.clear();
        if (failed != Status::Ok) { error = failure; return failed; }
        if (progress.phase == CheckpointRetentionPhase::Done) return Status::Ok;
        if (progress.phase != CheckpointRetentionPhase::Initialize) {
            const auto status = stableTip(error);
            if (status != Status::Ok) return status;
        }
        const auto resolver = [this](uint32_t h, uint256& hash) { return resolve(h, hash); };

        switch (progress.phase) {
        case CheckpointRetentionPhase::Initialize: {
            if (!policy.historical_interval || !policy.replay_blocks_per_step ||
                !policy.delete_heights_per_batch || !policy.recent_blocks) {
                return fail(Status::Invalid, "retention-policy-requires-positive-bounds", error);
            }
            const auto current_tip = db.getTip();
            if (!current_tip.ok()) return fail(current_tip.status(), "retention-tip-missing", error);
            tip = current_tip.value();
            if (tip.height < 0) return fail(Status::Invalid, "retention-negative-tip", error);
            if (stableTip(error) != Status::Ok) return failed;
            if (static_cast<uint32_t>(tip.height) <= policy.recent_blocks) {
                progress.phase = CheckpointRetentionPhase::Done;
                return Status::Ok;
            }
            const auto earliest = db.getEarliestUtreexoCheckpoint();
            const auto boundary = db.getLatestUtreexoCheckpointAtOrBelow(
                tip.height - static_cast<int64_t>(policy.recent_blocks));
            if (earliest.status() == Status::NotFound || boundary.status() == Status::NotFound) {
                progress.phase = CheckpointRetentionPhase::Done;
                return Status::Ok;
            }
            if (!earliest.ok() || !boundary.ok()) {
                return fail(!earliest.ok() ? earliest.status() : boundary.status(),
                            "retention-anchor-discovery-failed", error);
            }
            if (earliest.value().first < 0 || boundary.value().first < 0) {
                return fail(Status::Corruption, "retention-negative-anchor", error);
            }
            first = earliest.value().first;
            last = boundary.value().first;
            if (first >= last) {
                progress.phase = CheckpointRetentionPhase::Done;
                return Status::Ok;
            }
            probe = first;
            progress.interval_start = first;
            const auto prebase_marker = db.getPreBaseCoinSetBase();
            if (prebase_marker.ok()) {
                prebase = prebase_marker.value();
                if (prebase->second > static_cast<uint32_t>(tip.height)) {
                    return fail(Status::Invalid, "retention-prebase-marker-above-tip", error);
                }
                policy.protected_heights.push_back(prebase->second);
            } else if (prebase_marker.status() != Status::NotFound) {
                return fail(prebase_marker.status(), "retention-prebase-marker-read-failed", error);
            }
            std::sort(policy.protected_heights.begin(), policy.protected_heights.end());
            policy.protected_heights.erase(
                std::unique(policy.protected_heights.begin(), policy.protected_heights.end()),
                policy.protected_heights.end());
            ancestry.resize(static_cast<size_t>(tip.height) - first + 1);
            walk_height = tip.height;
            walk_hash = tip.hash;
            progress.phase = CheckpointRetentionPhase::Ancestry;
            return Status::Ok;
        }
        case CheckpointRetentionPhase::Ancestry: {
            for (uint32_t n = 0; n < policy.replay_blocks_per_step && walk_height >= first; ++n) {
                const auto indexed = db.getBlockHashByHeight(static_cast<int>(walk_height));
                if (!indexed.ok() || indexed.value() != walk_hash) {
                    return fail(Status::Invalid, "retention-ancestry-index-disagrees-at-" +
                                std::to_string(walk_height), error);
                }
                const auto header = db.getHeader(walk_hash);
                if (!header.ok()) {
                    return fail(header.status(), "retention-ancestry-header-missing-at-" +
                                std::to_string(walk_height), error);
                }
                ancestry[static_cast<size_t>(walk_height) - first] = walk_hash;
                walk_hash = header.value().prev_block_hash;
                --walk_height;
            }
            if (walk_height < first) {
                if (prebase && prebase->second >= first &&
                    ancestry[prebase->second - first] != prebase->first) {
                    return fail(Status::Invalid, "retention-prebase-marker-off-chain", error);
                }
                progress.phase = CheckpointRetentionPhase::SelectInterval;
            }
            return Status::Ok;
        }
        case CheckpointRetentionPhase::SelectInterval: {
            if (probe >= last || progress.interval_start >= last) {
                progress.phase = CheckpointRetentionPhase::Done;
                return Status::Ok;
            }
            uint64_t requested = std::min<uint64_t>(
                (uint64_t(probe) / policy.historical_interval + 1) * policy.historical_interval, last);
            const auto protected_it = std::upper_bound(
                policy.protected_heights.begin(), policy.protected_heights.end(), probe);
            if (protected_it != policy.protected_heights.end()) requested = std::min<uint64_t>(requested, *protected_it);
            probe = static_cast<uint32_t>(requested);
            const auto endpoint = db.getLatestUtreexoCheckpointAtOrBelow(static_cast<int>(probe));
            if (!endpoint.ok()) return fail(endpoint.status(), "retention-endpoint-read-failed", error);
            if (endpoint.value().first <= static_cast<int>(progress.interval_start)) return Status::Ok;
            progress.interval_end = static_cast<uint32_t>(endpoint.value().first);
            std::string reason;
            if (ReadAnchor(db, progress.interval_start, left, reason) != Status::Ok ||
                ReadAnchor(db, progress.interval_end, right, reason) != Status::Ok) {
                skipInterval(reason);
                return Status::Ok;
            }
            forest = std::make_unique<consensus::UtreexoForest>();
            if (RestoreHistoricalForest(db, progress.interval_start, *forest, reason, resolver) != Status::Ok) {
                skipInterval(reason);
                return Status::Ok;
            }
            progress.replayed_height = progress.interval_start;
            progress.phase = CheckpointRetentionPhase::VerifyInterval;
            return Status::Ok;
        }
        case CheckpointRetentionPhase::VerifyInterval: {
            for (uint32_t n = 0; n < policy.replay_blocks_per_step &&
                                 progress.replayed_height < progress.interval_end; ++n) {
                const uint32_t h = progress.replayed_height + 1;
                std::string reason;
                if (linkedHeight(h, reason) != Status::Ok ||
                    ReplayUtreexoDeltaRange(db, *forest, h - 1, h, reason, resolver) != Status::Ok) {
                    skipInterval(reason);
                    return Status::Ok;
                }
                progress.replayed_height = h;
                ++progress.replayed_blocks;
            }
            if (progress.replayed_height == progress.interval_end) {
                std::string reason;
                const auto binding_status = boundAnchors(reason);
                if (binding_status != Status::Ok) return fail(binding_status, reason, error);
                consensus::UtreexoForest expected;
                if (RestoreHistoricalForest(db, progress.interval_end, expected, reason, resolver) != Status::Ok) {
                    skipInterval(reason);
                    return Status::Ok;
                }
                // Equal roots alone do not prove identical leaf positions and
                // proof generation. Compare canonical deserialized full state.
                if (forest->serialize() != expected.serialize()) {
                    skipInterval("retention-replayed-state-differs-at-" + std::to_string(progress.interval_end));
                    return Status::Ok;
                }
                ++progress.verified_intervals;
                progress.eligible_height_keys += progress.interval_end - progress.interval_start - 1;
                delete_cursor = progress.interval_start + 1;
                if (apply && delete_cursor < progress.interval_end) {
                    progress.phase = CheckpointRetentionPhase::DeleteInterval;
                } else {
                    nextInterval();
                }
            }
            return Status::Ok;
        }
        case CheckpointRetentionPhase::DeleteInterval: {
            const auto status = boundAnchors(error);
            if (status != Status::Ok) return fail(status, error, error);
            rocksdb::WriteBatch batch;
            const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(
                uint64_t(delete_cursor) + policy.delete_heights_per_batch, progress.interval_end));
            for (uint32_t h = delete_cursor; h < end; ++h) {
                const auto staged = db.deleteUtreexoCheckpointWithChecksum(token, static_cast<int>(h), &batch);
                if (staged != Status::Ok) return fail(staged, "retention-delete-staging-failed", error);
            }
            const auto written = db.writeBatch(token, std::move(batch), true);
            if (written != Status::Ok) return fail(written, "retention-delete-commit-failed", error);
            progress.deleted_height_keys += end - delete_cursor;
            delete_cursor = end;
            if (delete_cursor == progress.interval_end) nextInterval();
            return Status::Ok;
        }
        case CheckpointRetentionPhase::Done:
            return Status::Ok;
        }
        return fail(Status::Internal, "retention-invalid-phase", error);
    }
};

CheckpointRetentionPass::CheckpointRetentionPass(ChainDB& db, CheckpointRetentionPolicy policy, bool apply)
    : state_(std::make_unique<State>(db, std::move(policy), apply)) {}
CheckpointRetentionPass::~CheckpointRetentionPass() = default;
Status CheckpointRetentionPass::step(const ChainWriteToken& token, std::string& error) {
    return state_->step(token, error);
}
const CheckpointRetentionProgress& CheckpointRetentionPass::progress() const { return state_->progress; }
bool CheckpointRetentionPass::done() const {
    return state_->progress.phase == CheckpointRetentionPhase::Done || state_->failed != Status::Ok;
}
} // namespace dinero::storage
