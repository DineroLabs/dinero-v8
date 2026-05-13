#pragma once

#include "../../p2p/state_transition.h"
#include "../../storage/block_storage.h"

namespace dinero {
namespace consensus {
namespace adapters {

/**
 * UndoStorageAdapter - Adapts BlockStorage to p2p::IUndoStorage interface
 *
 * This is a zero-logic adapter that forwards calls between ChainManager's
 * BlockStorage and G.3.4's IUndoStorage interface. It performs only type conversion.
 *
 * ADAPTER RULES:
 * - No conditional logic
 * - No policy decisions
 * - No state of its own
 * - Pure mechanical forwarding
 */
class UndoStorageAdapter final : public p2p::IUndoStorage {
public:
    explicit UndoStorageAdapter(BlockStorage& block_storage)
        : block_storage_(block_storage) {}

    // Disable copy/move (adapter is a view, not a value)
    UndoStorageAdapter(const UndoStorageAdapter&) = delete;
    UndoStorageAdapter& operator=(const UndoStorageAdapter&) = delete;

    /**
     * Load block from disk storage (L2.3)
     *
     * Forwards to BlockStorage::readBlock().
     * Fails hard if block missing - no fallback, no retry.
     */
    bool loadBlock(uint32_t file_id,
                   uint64_t offset,
                   uint32_t size,
                   p2p::Block& out_block) const override {
        FilePosition pos(file_id, offset, size);

        auto result = block_storage_.readBlock(pos);
        if (result.status() != Status::Ok) {
            return false;  // Block not found or read error - fail hard
        }

        // TODO: Convert dinero::Block → p2p::Block
        // This requires implementing Block::Deserialize() first (currently stubbed)
        // For now, return false since BlockStorage::readBlock() is not yet implemented
        (void)result;  // Suppress unused warning
        return false;  // Not yet implemented
    }

    /**
     * Check if undo data exists for a block
     *
     * NOTE: BlockStorage doesn't have a hasUndo() method that takes just a hash.
     * It requires a FilePosition. For now, returns true (stub).
     * TODO: Track undo positions or add BlockStorage::hasUndo(hash) method.
     */
    bool hasUndo(const p2p::Hash256& block_hash) const override {
        // TODO: Need to track FilePosition for this hash, or add BlockStorage::hasUndo()
        (void)block_hash;
        return true;  // Stub - assume undo exists
    }

    /**
     * Write undo data to storage
     *
     * Converts: p2p::Hash256 → uint256, calls BlockStorage::writeUndo()
     * Extracts: FilePosition → (file_id, offset, length, checksum)
     */
    bool writeUndo(const p2p::Hash256& block_hash,
                   const std::vector<uint8_t>& data,
                   uint32_t& out_file_id,
                   uint64_t& out_offset,
                   uint64_t& out_length,
                   uint32_t& out_checksum) override {
        // Convert p2p::Hash256 to uint256
        uint256 hash = convertHash(block_hash);

        // Forward to BlockStorage::writeUndo()
        auto result = block_storage_.writeUndo(hash, data);
        if (result.status() != Status::Ok) {
            return false;
        }

        // Extract FilePosition into output parameters
        const FilePosition& pos = result.value();
        out_file_id = pos.file_number;
        out_offset = pos.offset;
        out_length = pos.size;  // FilePosition.size is the undo data length
        out_checksum = 0;  // TODO: BlockStorage should return checksum

        return true;
    }

    /**
     * Read undo data from storage
     *
     * Constructs: FilePosition from (file_id, offset, length, checksum)
     * Calls: BlockStorage::readUndo(FilePosition)
     */
    bool readUndo(const p2p::Hash256& block_hash,
                  uint32_t file_id,
                  uint64_t offset,
                  uint64_t length,
                  uint32_t expected_checksum,
                  std::vector<uint8_t>& out_data) override {
        // Construct FilePosition from input parameters
        FilePosition pos(file_id, offset, static_cast<uint32_t>(length));

        // Forward to BlockStorage::readUndo()
        auto result = block_storage_.readUndo(pos);
        if (result.status() != Status::Ok) {
            return false;
        }

        out_data = result.value();

        // TODO: Verify checksum (BlockStorage should do this internally)
        (void)expected_checksum;  // Unused for now
        (void)block_hash;  // Unused for now

        return true;
    }

private:
    /**
     * Convert p2p::Hash256 (32-byte array) to uint256 (32-byte array)
     *
     * Phase M.0: Direct byte copy - NO hex conversion (identity, not presentation)
     * This is pure mechanical conversion - no logic, no validation.
     */
    static uint256 convertHash(const p2p::Hash256& hash) {
        uint256 result;
        std::memcpy(result.data, hash.data.data(), 32);  // Direct byte copy
        return result;
    }

    BlockStorage& block_storage_;  // Reference to ChainManager's block storage
};

} // namespace adapters
} // namespace consensus
} // namespace dinero
