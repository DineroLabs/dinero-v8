#pragma once

#include "../../p2p/state_transition.h"
#include "../../consensus/block_index.h"

namespace dinero {
namespace consensus {
namespace adapters {

/**
 * BlockIndexAdapter - Provides p2p::BlockIndex view over consensus::CBlockIndex
 *
 * This adapter converts between ChainManager's CBlockIndex* and G.3.5's BlockIndex&.
 * It's a view (reference wrapper), not a copy.
 *
 * ADAPTER RULES:
 * - No conditional logic
 * - No policy decisions
 * - No state of its own (except the p2p::BlockIndex view)
 * - Pure mechanical forwarding
 *
 * IMPORTANT: The returned BlockIndex is a temporary view. Do not store pointers to it.
 */
class BlockIndexAdapter {
public:
    /**
     * Convert CBlockIndex* to BlockIndex&
     *
     * Creates a temporary BlockIndex structure that views the CBlockIndex data.
     * The returned reference is only valid as long as this adapter and the
     * underlying CBlockIndex exist.
     */
    static p2p::BlockIndex& toBlockIndex(CBlockIndex* block_index, p2p::BlockIndex& out_index) {
        if (!block_index) {
            // Return empty BlockIndex for null input
            out_index = p2p::BlockIndex();
            return out_index;
        }

        // Convert CBlockIndex fields to BlockIndex fields
        // Hash conversion: uint256 → hex string → Hash256
        out_index.hash = convertHashToBytes(block_index->hash.GetHex());  // Phase M.0: uint256 → string
        out_index.prev_hash = convertHashToBytes(block_index->prev_hash.GetHex());  // Phase M.0: uint256 → string
        out_index.height = block_index->height;

        // Convert chainwork: string → uint64_t
        // CBlockIndex stores chainwork as hex string, BlockIndex as uint64
        out_index.chainwork = convertChainwork(block_index->chainwork);

        // Status flags
        out_index.connected = (block_index->status & BLOCK_VALID_SCRIPTS) != 0;

        // Undo data location (Phase M.0: direct field access, not struct)
        out_index.undo_file_id = block_index->undo_file;
        out_index.undo_file_offset = block_index->undo_pos;
        out_index.undo_length = block_index->undo_size;
        out_index.undo_checksum = 0;  // CBlockIndex doesn't store checksum

        return out_index;
    }

private:
    /**
     * Convert hex string to Hash256 (32-byte array)
     */
    static p2p::Hash256 convertHashToBytes(const std::string& hex) {
        p2p::Hash256 hash;
        if (hex.length() != 64) {
            // Invalid hex string - return zero hash
            hash.data.fill(0);
            return hash;
        }

        for (size_t i = 0; i < 32; i++) {
            unsigned int byte;
            sscanf(&hex[i * 2], "%02x", &byte);
            hash.data[i] = static_cast<uint8_t>(byte);
        }
        return hash;
    }

    /**
     * Convert chainwork hex string to uint64_t
     *
     * CBlockIndex stores chainwork as full-precision hex string.
     * BlockIndex stores as uint64 (approximation for comparison).
     * This takes the last 16 hex chars (64 bits).
     */
    static uint64_t convertChainwork(const std::string& hex) {
        if (hex.empty()) return 0;

        // Take last 16 hex characters (64 bits)
        size_t start = hex.length() > 16 ? hex.length() - 16 : 0;
        std::string last_64_bits = hex.substr(start);

        // Convert hex to uint64
        uint64_t chainwork = 0;
        sscanf(last_64_bits.c_str(), "%016llx", &chainwork);
        return chainwork;
    }
};

} // namespace adapters
} // namespace consensus
} // namespace dinero
