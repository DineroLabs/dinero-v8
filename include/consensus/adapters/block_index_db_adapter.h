#pragma once

#include "../../p2p/state_transition.h"
#include "../../storage/chain_db.h"

namespace dinero {
namespace consensus {
namespace adapters {

/**
 * BlockIndexDBAdapter - Adapts ChainDB to p2p::IBlockIndexDB interface
 *
 * This is a zero-logic adapter that forwards calls between ChainManager's
 * ChainDB and G.3.4's IBlockIndexDB interface.
 *
 * ADAPTER RULES:
 * - No conditional logic
 * - No policy decisions
 * - No state of its own
 * - Pure mechanical forwarding
 *
 * STATUS: Stub implementation - ChainDB interface doesn't exactly match IBlockIndexDB
 * REQUIRED: ChainDB needs isBlockConnected/markBlockConnected methods added
 */
class BlockIndexDBAdapter final : public p2p::IBlockIndexDB {
public:
    explicit BlockIndexDBAdapter(ChainDB& chain_db) : chain_db_(chain_db) {}

    // Disable copy/move (adapter is a view, not a value)
    BlockIndexDBAdapter(const BlockIndexDBAdapter&) = delete;
    BlockIndexDBAdapter& operator=(const BlockIndexDBAdapter&) = delete;

    /**
     * Check if block is connected (BLOCK_CONNECTED flag set)
     *
     * Forwards to ChainDB::isBlockConnected().
     * Pure mechanical forwarding - no logic.
     */
    bool isBlockConnected(const p2p::Hash256& block_hash) const override {
        uint256 hash = convertHash(block_hash);
        return chain_db_.isBlockConnected(hash);
    }

    /**
     * Mark block as connected
     *
     * Forwards to ChainDB::markBlockConnected().
     * Pure mechanical forwarding - no logic.
     */
    void markBlockConnected(const p2p::Hash256& block_hash, bool connected) override {
        uint256 hash = convertHash(block_hash);
        chain_db_.markBlockConnected(hash, connected);
    }

    /**
     * Get block index by hash
     *
     * Forwards to ChainDB::getBlockIndex().
     * Pure mechanical forwarding - no logic.
     *
     * NOTE: Returns nullptr for now (ChainDB stub implementation).
     */
    p2p::BlockIndex* getBlockIndex(const p2p::Hash256& block_hash) override {
        uint256 hash = convertHash(block_hash);
        CBlockIndex* cblock_index = chain_db_.getBlockIndex(hash);

        // TODO: Convert CBlockIndex* to p2p::BlockIndex* using BlockIndexAdapter
        // For now, returns nullptr since ChainDB returns nullptr
        (void)cblock_index;
        return nullptr;
    }

    /**
     * Commit pending changes to database
     *
     * Forwards to ChainDB::commitBatch().
     * Pure mechanical forwarding - no logic.
     */
    bool commitBatch() override {
        return chain_db_.commitBatch();
    }

    /**
     * Update block undo position (L2.4: Persist undo positions)
     *
     * Forwards to ChainDB::getBlockIndex() + in-memory update.
     * Note: Changes are in-memory only until caller persists via updateBlockIndex.
     */
    void setBlockUndoPosition(const p2p::Hash256& block_hash,
                              uint32_t file_id,
                              uint64_t offset,
                              uint64_t length,
                              uint32_t checksum) override {
        uint256 hash = convertHash(block_hash);
        CBlockIndex* pindex = chain_db_.getBlockIndex(hash);

        if (pindex) {
            // Update in-memory CBlockIndex
            // Note: ChainDB::updateBlockIndex() must be called later to persist
            pindex->undo_file = file_id;        // Phase M.0: Correct field name
            pindex->undo_pos = offset;          // Phase M.0: Correct field name
            pindex->undo_size = length;         // Phase M.0: Correct field name
            // Note: checksum not stored in CBlockIndex (verified during read)
        }
        // Silently ignore if block not found (defensive)
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

    ChainDB& chain_db_;  // Reference to ChainManager's database
};

} // namespace adapters
} // namespace consensus
} // namespace dinero
