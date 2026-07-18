#include "consensus/utreexo_delta_codec.h"

#include "common/serialization.h"
#include "consensus/utreexo_accumulator.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <limits>

namespace dinero {

namespace {
constexpr const char* kUtreexoDeltaUndoPrefix = "UD:";
constexpr uint8_t kUtreexoDeltaUndoSchemaV1 = 1;
constexpr size_t kUtreexoLeafHashSize = 32;
}  // namespace

std::string MakeUtreexoDeltaUndoKey(const uint256& block_hash) {
    return std::string(kUtreexoDeltaUndoPrefix) + block_hash.GetHex();
}

bool SerializeUtreexoDelta(const consensus::UtreexoDelta& delta, std::string& out, std::string& error) {
    try {
        VectorWriter writer;
        writer.write(kUtreexoDeltaUndoSchemaV1);
        writer.write(delta.numLeavesBefore);
        writer.writeVarInt(delta.deletedLeaves.size());
        for (const auto& deleted : delta.deletedLeaves) {
            if (deleted.leafHash.size() != kUtreexoLeafHashSize) {
                error = "utreexo-delta-serialize-invalid-deleted-hash-size";
                return false;
            }
            writer.write(deleted.leafHash.data(), kUtreexoLeafHashSize);
            writer.write(deleted.position);
        }

        writer.writeVarInt(delta.addedLeaves.size());
        for (const auto& added : delta.addedLeaves) {
            if (added.hash.size() != kUtreexoLeafHashSize) {
                error = "utreexo-delta-serialize-invalid-added-hash-size";
                return false;
            }
            writer.write(added.hash.data(), kUtreexoLeafHashSize);
            writer.write(added.position);
        }

        out = writer.release_string();
        return true;
    } catch (const std::exception& e) {
        error = std::string("utreexo-delta-serialize-exception: ") + e.what();
        return false;
    }
}

bool DeserializeUtreexoDelta(const std::string& data, consensus::UtreexoDelta& out, std::string& error) {
    try {
        Reader reader(data);
        const uint8_t schema = reader.read<uint8_t>();
        if (schema != kUtreexoDeltaUndoSchemaV1) {
            error = "utreexo-delta-unsupported-schema";
            return false;
        }

        consensus::UtreexoDelta parsed;
        parsed.numLeavesBefore = reader.read<uint64_t>();

        const uint64_t deleted_count = reader.readVarInt();
        parsed.deletedLeaves.reserve(static_cast<size_t>(deleted_count));
        for (uint64_t i = 0; i < deleted_count; ++i) {
            consensus::UtreexoHash hash(kUtreexoLeafHashSize);
            reader.read(hash.data(), kUtreexoLeafHashSize);
            const uint64_t position = reader.read<uint64_t>();
            parsed.deletedLeaves.emplace_back(hash, position);
        }

        const uint64_t added_count = reader.readVarInt();
        parsed.addedLeaves.reserve(static_cast<size_t>(added_count));
        for (uint64_t i = 0; i < added_count; ++i) {
            consensus::UtreexoHash hash(kUtreexoLeafHashSize);
            reader.read(hash.data(), kUtreexoLeafHashSize);
            const uint64_t position = reader.read<uint64_t>();
            parsed.addedLeaves.emplace_back(hash, position);
        }

        if (!reader.eof()) {
            error = "utreexo-delta-trailing-bytes";
            return false;
        }

        out = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        error = std::string("utreexo-delta-deserialize-exception: ") + e.what();
        return false;
    }
}

bool ApplyUtreexoDeltaForward(consensus::UtreexoForest& forest,
                              const consensus::UtreexoDelta& delta,
                              std::string& error) {
    if (forest.getNumLeaves() != delta.numLeavesBefore) {
        error = "utreexo-delta-replay-pre-state-mismatch: forest has " +
                std::to_string(forest.getNumLeaves()) + " leaves, delta expects " +
                std::to_string(delta.numLeavesBefore);
        return false;
    }

    for (size_t i = 0; i < delta.deletedLeaves.size(); ++i) {
        const auto& deleted = delta.deletedLeaves[i];
        if (!forest.removeAtKnownPosition(deleted.position, deleted.leafHash)) {
            error = "utreexo-delta-replay-remove-failed at delete " +
                    std::to_string(i) + " position " +
                    std::to_string(deleted.position);
            return false;
        }
    }

    for (size_t i = 0; i < delta.addedLeaves.size(); ++i) {
        const auto& added = delta.addedLeaves[i];
        const uint64_t position = forest.add(added.hash);
        if (position == UINT64_MAX) {
            error = "utreexo-delta-replay-add-failed at add " + std::to_string(i);
            return false;
        }
        if (position != added.position) {
            error = "utreexo-delta-replay-position-drift at add " +
                    std::to_string(i) + ": got " + std::to_string(position) +
                    " recorded " + std::to_string(added.position);
            return false;
        }
    }

    return true;
}

bool BuildStatelessUtreexoDelta(
    const consensus::UtreexoForest& forest_before,
    const Block& block,
    uint32_t block_height,
    const std::vector<consensus::UtreexoHash>& spend_targets,
    consensus::UtreexoDelta& out,
    std::string& error) {
    consensus::UtreexoDelta built;
    built.numLeavesBefore = forest_before.getNumLeaves();

    for (size_t i = 0; i < spend_targets.size(); ++i) {
        const auto position = forest_before.findLeafPosition(spend_targets[i]);
        if (!position.has_value()) {
            error = "stateless-delta-target-missing-at-index-" +
                    std::to_string(i);
            return false;
        }
        built.recordDelete(*position, spend_targets[i]);
    }

    const auto additions =
        consensus::UtreexoTransitionProof::computeAdditionHashes(
            block, block_height);
    if (additions.size() >
        std::numeric_limits<uint64_t>::max() - built.numLeavesBefore) {
        error = "stateless-delta-add-position-overflow";
        return false;
    }
    for (size_t i = 0; i < additions.size(); ++i) {
        built.recordAdd(
            additions[i], built.numLeavesBefore + static_cast<uint64_t>(i));
    }

    out = std::move(built);
    return true;
}

}  // namespace dinero
