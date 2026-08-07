#pragma once

#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "consensus/block_lifecycle.h"

#include <limits>

namespace dinero::storage {

enum class ArchivalReadMode {
    AllowLegacyFallback = 0,
    RequireFlatfiles
};

enum class ArchivalReadSource {
    None = 0,
    Flatfile,
    LegacyChainDB
};

struct StrictFlatfileCoverageResult {
    bool ok{false};
    uint64_t expected_body_count{0};
    uint64_t verified_body_count{0};
    int first_missing_height{-1};
    uint256 first_missing_hash;
    std::string error;
};

template <typename T>
struct ArchivalReadOutcome {
    StatusOr<T> result;
    ArchivalReadSource source{ArchivalReadSource::None};
};

inline bool LegacyFallbackAllowed(ArchivalReadMode mode) {
    return mode == ArchivalReadMode::AllowLegacyFallback;
}

inline bool HasArchivalBlockBody(const ChainDB& chain_db,
                                 const BlockStorage* block_storage,
                                 const uint256& hash,
                                 ArchivalReadMode mode = ArchivalReadMode::AllowLegacyFallback) {
    if (!block_storage) {
        if (!LegacyFallbackAllowed(mode)) {
            return false;
        }
        return chain_db.hasBlock(hash) == Status::Ok;
    }

    auto metadata_result = chain_db.getHeaderMetadata(hash);
    if (metadata_result.status() == Status::Ok) {
        const auto& metadata = metadata_result.value();
        if (metadata.data_size > 0) {
            const FilePosition pos(metadata.file_number, metadata.data_pos, metadata.data_size);
            if (block_storage->hasBlock(pos) == Status::Ok) {
                return true;
            }
        }
    }

    if (!LegacyFallbackAllowed(mode)) {
        return false;
    }

    return chain_db.hasBlock(hash) == Status::Ok;
}

inline ArchivalReadOutcome<Block> ReadArchivalBlockDetailed(
    const ChainDB& chain_db,
    const BlockStorage* block_storage,
    const uint256& hash,
    ArchivalReadMode mode = ArchivalReadMode::AllowLegacyFallback) {
    if (block_storage) {
        auto metadata_result = chain_db.getHeaderMetadata(hash);
        if (metadata_result.status() == Status::Ok) {
            const auto& metadata = metadata_result.value();
            if (metadata.data_size > 0) {
                const FilePosition pos(metadata.file_number, metadata.data_pos, metadata.data_size);
                auto block_result = block_storage->readBlock(pos);
                if (block_result.status() == Status::Ok) {
                    if (block_result.value().GetHash() != hash) {
                        if (!LegacyFallbackAllowed(mode)) {
                            return {Status::Corruption, ArchivalReadSource::None};
                        }
                    } else {
                    return {std::move(block_result), ArchivalReadSource::Flatfile};
                    }
                }
                if (!LegacyFallbackAllowed(mode)) {
                    return {block_result.status(), ArchivalReadSource::None};
                }
            }
        }
    } else if (!LegacyFallbackAllowed(mode)) {
        return {Status::Internal, ArchivalReadSource::None};
    }

    if (!LegacyFallbackAllowed(mode)) {
        return {Status::NotFound, ArchivalReadSource::None};
    }

    auto block_result = chain_db.getBlock(hash);
    const bool used_legacy_fallback = block_result.ok();
    return {std::move(block_result),
            used_legacy_fallback ? ArchivalReadSource::LegacyChainDB : ArchivalReadSource::None};
}

inline StatusOr<Block> ReadArchivalBlock(const ChainDB& chain_db,
                                         const BlockStorage* block_storage,
                                         const uint256& hash,
                                         ArchivalReadMode mode = ArchivalReadMode::AllowLegacyFallback) {
    return ReadArchivalBlockDetailed(chain_db, block_storage, hash, mode).result;
}

enum class BodyPositionPersistResult {
    Stored,
    ReplacedStale,
    RetainedReadable
};

// Record a newly-written, already-verified body position for an existing
// header. A HAVE_DATA bit is only a claim: interrupted writes and stale
// flatfile references can leave it pointing at a missing, corrupt, or wrong
// block. Preserve an existing position only when a strict read resolves to the
// requested hash; otherwise replace it with the scheduler's verified copy.
//
// Status::NotFound means the header metadata row does not exist. The caller
// that owns the in-memory block index may synthesize that row; storage code
// deliberately does not know how to do so.
inline StatusOr<BodyPositionPersistResult> PersistVerifiedArchivalBodyPosition(
    ChainDB& chain_db,
    const BlockStorage* block_storage,
    const uint256& hash,
    const FilePosition& pos) {
    if (pos.offset > std::numeric_limits<uint32_t>::max()) {
        return Status::Invalid;
    }

    auto metadata_result = chain_db.getHeaderMetadata(hash);
    if (metadata_result.status() != Status::Ok) {
        return metadata_result.status();
    }

    auto metadata = metadata_result.value();
    const bool claimed_existing_body =
        (metadata.status_flags & BLOCK_HAVE_DATA) != 0 && metadata.data_size > 0;
    if (claimed_existing_body) {
        const auto existing = ReadArchivalBlockDetailed(
            chain_db, block_storage, hash, ArchivalReadMode::RequireFlatfiles);
        if (existing.result.status() == Status::Ok &&
            existing.result.value().GetHash() == hash) {
            return BodyPositionPersistResult::RetainedReadable;
        }
    }

    metadata.file_number = pos.file_number;
    metadata.data_pos = static_cast<uint32_t>(pos.offset);
    metadata.data_size = pos.size;
    metadata.status_flags |= BLOCK_HAVE_DATA;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const Status status = chain_db.putHeaderMetadataPreservingExistingUndo(
        token, hash, metadata, nullptr);
    if (status != Status::Ok) {
        return status;
    }
    return claimed_existing_body ? BodyPositionPersistResult::ReplacedStale
                                 : BodyPositionPersistResult::Stored;
}

inline ArchivalReadOutcome<UndoRecord> ReadArchivalUndoDetailed(
    const ChainDB& chain_db,
    const BlockStorage* block_storage,
    const uint256& hash,
    ArchivalReadMode mode = ArchivalReadMode::AllowLegacyFallback) {
    if (block_storage) {
        auto metadata_result = chain_db.getHeaderMetadata(hash);
        if (metadata_result.status() == Status::Ok) {
            const auto& metadata = metadata_result.value();
            if (metadata.undo_size > 0) {
                const FilePosition pos(metadata.undo_file, metadata.undo_pos, metadata.undo_size);
                auto undo_bytes_result = block_storage->readUndo(pos);
                if (undo_bytes_result.status() == Status::Ok) {
                    try {
                        return {UndoRecord::Deserialize(undo_bytes_result.value()),
                                ArchivalReadSource::Flatfile};
                    } catch (const std::exception&) {
                        if (!LegacyFallbackAllowed(mode)) {
                            return {Status::Serialization, ArchivalReadSource::None};
                        }
                    }
                } else if (!LegacyFallbackAllowed(mode)) {
                    return {undo_bytes_result.status(), ArchivalReadSource::None};
                }
            }
        }
    } else if (!LegacyFallbackAllowed(mode)) {
        return {Status::Internal, ArchivalReadSource::None};
    }

    if (!LegacyFallbackAllowed(mode)) {
        return {Status::NotFound, ArchivalReadSource::None};
    }

    auto undo_result = chain_db.getUndo(hash);
    const bool used_legacy_fallback = undo_result.ok();
    return {std::move(undo_result),
            used_legacy_fallback ? ArchivalReadSource::LegacyChainDB : ArchivalReadSource::None};
}

inline StatusOr<UndoRecord> ReadArchivalUndo(const ChainDB& chain_db,
                                             const BlockStorage* block_storage,
                                             const uint256& hash,
                                             ArchivalReadMode mode = ArchivalReadMode::AllowLegacyFallback) {
    return ReadArchivalUndoDetailed(chain_db, block_storage, hash, mode).result;
}

inline StrictFlatfileCoverageResult VerifyStrictFlatfileCoverage(
    const ChainDB& chain_db,
    const BlockStorage* block_storage,
    uint32_t tip_height,
    bool prune_mode_enabled = false,
    uint32_t prune_height = 0) {
    StrictFlatfileCoverageResult result;
    result.expected_body_count = static_cast<uint64_t>(tip_height) + 1;

    if (!block_storage) {
        result.error = "strict archival mode requires BlockStorage";
        return result;
    }

    if (prune_mode_enabled) {
        result.error = "strict archival mode cannot start on a pruned datadir";
        return result;
    }

    if (prune_height > 0) {
        result.error = "strict archival mode requires full genesis coverage, but prune_height=" +
                       std::to_string(prune_height);
        return result;
    }

    for (uint32_t height = 0; height <= tip_height; ++height) {
        auto hash_result = chain_db.getBlockHashByHeight(static_cast<int>(height));
        if (hash_result.status() != Status::Ok) {
            result.first_missing_height = static_cast<int>(height);
            result.error = "missing height index entry at height " + std::to_string(height);
            return result;
        }

        const auto& hash = hash_result.value();
        if (!HasArchivalBlockBody(
                chain_db,
                block_storage,
                hash,
                ArchivalReadMode::RequireFlatfiles)) {
            result.first_missing_height = static_cast<int>(height);
            result.first_missing_hash = hash;
            result.error = "missing flatfile block body at height " + std::to_string(height) +
                           " hash=" + hash.GetHex();
            return result;
        }

        result.verified_body_count++;
    }

    result.ok = true;
    return result;
}

}  // namespace dinero::storage
