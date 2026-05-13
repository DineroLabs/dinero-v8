/**
 * Phase G.3.4: State Transition Implementation (TEST SCAFFOLDING ONLY)
 *
 * ⚠️  WARNING: This is test infrastructure, not production consensus code.
 * ⚠️  Uses fake block hashes for testing state transition flows.
 * ⚠️  Must never be compiled into production binaries.
 *
 * Applies validated blocks to chainstate (UTXO set + block index).
 * This is the ONLY module that mutates consensus-critical state.
 */

#ifndef DINERO_TESTING
#error "state_transition.cpp is test-only scaffolding and must not be built in production"
#endif

#include "../../include/p2p/state_transition.h"
#include <algorithm>

namespace dinero {
namespace p2p {

//=============================================================================
// Helper: Serialize Undo Data
//=============================================================================

/**
 * Serialize undo data for a block
 *
 * Format (simplified for initial implementation):
 * - For each non-coinbase transaction:
 *   - For each input: serialize the spent UTXO (scriptPubKey + value)
 *
 * Bitcoin Core format is more complex (includes height, coinbase flag, etc.)
 * but this simplified format is sufficient for testing the state transition logic.
 */
std::vector<uint8_t> serializeUndoData(const Block& block, const IUTXOView& utxo_view) {
    std::vector<uint8_t> undo_data;

    // For each non-coinbase transaction
    for (size_t tx_idx = 1; tx_idx < block.transactions.size(); tx_idx++) {
        const Transaction& tx = block.transactions[tx_idx];

        // For each input, save the UTXO being spent
        for (const auto& input : tx.inputs) {
            auto utxo_opt = utxo_view.getUTXO(input.prevout);
            if (!utxo_opt.has_value()) {
                // This should never happen if validation passed
                continue;
            }

            const TxOut& utxo = utxo_opt.value();

            // Serialize: value (8 bytes) + scriptPubKey length (4 bytes) + scriptPubKey
            uint64_t value = utxo.value;
            undo_data.push_back((value >> 0) & 0xFF);
            undo_data.push_back((value >> 8) & 0xFF);
            undo_data.push_back((value >> 16) & 0xFF);
            undo_data.push_back((value >> 24) & 0xFF);
            undo_data.push_back((value >> 32) & 0xFF);
            undo_data.push_back((value >> 40) & 0xFF);
            undo_data.push_back((value >> 48) & 0xFF);
            undo_data.push_back((value >> 56) & 0xFF);

            uint32_t script_len = utxo.scriptPubKey.size();
            undo_data.push_back((script_len >> 0) & 0xFF);
            undo_data.push_back((script_len >> 8) & 0xFF);
            undo_data.push_back((script_len >> 16) & 0xFF);
            undo_data.push_back((script_len >> 24) & 0xFF);

            undo_data.insert(undo_data.end(), utxo.scriptPubKey.begin(), utxo.scriptPubKey.end());
        }
    }

    return undo_data;
}

//=============================================================================
// Helper: Deserialize Undo Data
//=============================================================================

/**
 * Deserialize undo data for a block
 *
 * Returns a list of UTXOs in the order they were spent
 */
std::vector<TxOut> deserializeUndoData(const std::vector<uint8_t>& undo_data) {
    std::vector<TxOut> spent_utxos;

    size_t pos = 0;
    while (pos + 12 <= undo_data.size()) {  // At least value (8) + script_len (4)
        // Read value (8 bytes)
        uint64_t value = 0;
        value |= (uint64_t)undo_data[pos++] << 0;
        value |= (uint64_t)undo_data[pos++] << 8;
        value |= (uint64_t)undo_data[pos++] << 16;
        value |= (uint64_t)undo_data[pos++] << 24;
        value |= (uint64_t)undo_data[pos++] << 32;
        value |= (uint64_t)undo_data[pos++] << 40;
        value |= (uint64_t)undo_data[pos++] << 48;
        value |= (uint64_t)undo_data[pos++] << 56;

        // Read script length (4 bytes)
        uint32_t script_len = 0;
        script_len |= (uint32_t)undo_data[pos++] << 0;
        script_len |= (uint32_t)undo_data[pos++] << 8;
        script_len |= (uint32_t)undo_data[pos++] << 16;
        script_len |= (uint32_t)undo_data[pos++] << 24;

        if (pos + script_len > undo_data.size()) {
            break;  // Truncated data
        }

        // Read scriptPubKey
        std::vector<uint8_t> script_pubkey(undo_data.begin() + pos,
                                            undo_data.begin() + pos + script_len);
        pos += script_len;

        TxOut utxo;
        utxo.value = value;
        utxo.scriptPubKey = script_pubkey;
        spent_utxos.push_back(utxo);
    }

    return spent_utxos;
}

//=============================================================================
// Helper: Calculate Block Hash (Simplified)
//=============================================================================

/**
 * Generate fake block identifier for testing (NOT A REAL HASH)
 *
 * ⚠️  ARCHITECTURAL LIE: This is NOT a cryptographic hash.
 * ⚠️  This creates fake identity for test scaffolding only.
 *
 * In production, block identity MUST be the actual SHA256d hash of the block header.
 * This function exists only to exercise state transition logic in tests.
 */
Hash256 generateFakeTestBlockId(const Block& block, uint32_t height) {
    Hash256 hash;
    // Use first transaction's first output value if available (makes blocks at same height distinguishable)
    if (!block.transactions.empty() && !block.transactions[0].outputs.empty()) {
        uint64_t value = block.transactions[0].outputs[0].value;
        hash.data[0] = (uint8_t)(value & 0xFF);
        hash.data[1] = (uint8_t)((value >> 8) & 0xFF);
        hash.data[2] = (uint8_t)((value >> 16) & 0xFF);
        hash.data[3] = (uint8_t)((value >> 24) & 0xFF);
    } else {
        // Fallback: use height
        hash.data[0] = (uint8_t)(height & 0xFF);
        hash.data[1] = (uint8_t)((height >> 8) & 0xFF);
    }
    return hash;
}

//=============================================================================
// ConnectBlock Implementation
//=============================================================================

BlockConnectionResult ConnectBlock(
    const Block& block,
    uint32_t height,
    IUTXOView& utxo_view,
    IBlockIndexDB& block_index_db,
    IUndoStorage& undo_storage,
    const ConsensusParams& params
) {
    (void)params;  // Unused in G.3.4 (for future BIP activations)

    // Calculate block hash
    Hash256 block_hash = generateFakeTestBlockId(block, height);

    // PRECONDITION CHECK: Block must not already be connected
    if (block_index_db.isBlockConnected(block_hash)) {
        return BlockConnectionResult::Fail(
            "Block already connected (BLOCK_CONNECTED flag set)",
            ConnectFailReason::PRECONDITION
        );
    }

    // PHASE A: UTXO Snapshot + Undo Data Serialization
    // =================================================

    // Step 1: Serialize undo data from UNMODIFIED UTXO view
    // CRITICAL: utxo_view MUST NOT be mutated before undo serialization
    // Undo data captures the state BEFORE this block's mutations
    const IUTXOView& utxo_snapshot = utxo_view;  // Read-only snapshot
    std::vector<uint8_t> undo_data = serializeUndoData(block, utxo_snapshot);

    // Step 2: Buffer UTXO mutations (don't apply yet - wait for DB commit)
    // This ensures all-or-nothing observable state
    std::vector<OutPoint> utxos_to_remove;
    std::vector<std::pair<OutPoint, TxOut>> utxos_to_add;

    for (size_t tx_idx = 0; tx_idx < block.transactions.size(); tx_idx++) {
        const Transaction& tx = block.transactions[tx_idx];

        // Buffer spent inputs for removal (except coinbase)
        if (!tx.isCoinbase()) {
            for (const auto& input : tx.inputs) {
                utxos_to_remove.push_back(input.prevout);
            }
        }

        // Buffer new outputs for addition
        for (size_t out_idx = 0; out_idx < tx.outputs.size(); out_idx++) {
            OutPoint outpoint;
            // Convert Hash256 to uint256, then wrap in TxId - Phase M.4.3-B
            uint256 txid_raw;
            std::memcpy(txid_raw.data, block_hash.data.data(), 32);
            outpoint.txid = TxId(txid_raw);
            outpoint.vout = out_idx;

            utxos_to_add.push_back({outpoint, tx.outputs[out_idx]});
        }
    }

    // PHASE B: Undo Data Persistence (Filesystem Durability)
    // =======================================================

    uint32_t undo_file_id = 0;
    uint64_t undo_offset = 0;
    uint64_t undo_length = 0;
    uint32_t undo_checksum = 0;

    bool undo_write_ok = undo_storage.writeUndo(
        block_hash,
        undo_data,
        undo_file_id,
        undo_offset,
        undo_length,
        undo_checksum
    );

    if (!undo_write_ok) {
        // Undo write failed - no UTXO mutations applied yet
        // Observable state unchanged (crash-safe)
        return BlockConnectionResult::Fail(
            "Failed to write undo data",
            ConnectFailReason::UNDO_IO
        );
    }

    // PHASE C: Database Commit (RocksDB Atomic Batch)
    // ===============================================

    // Commit all changes atomically
    // In production, this would include:
    // - UTXO updates (add/delete)
    // - Block index metadata (status, height, chainwork)
    // - Chainstate metadata (best block, height)
    // - Undo mapping (file_id, offset, length, checksum)

    bool db_commit_ok = block_index_db.commitBatch();

    if (!db_commit_ok) {
        // DB commit failed - no UTXO mutations applied yet
        // Undo data exists on disk (orphaned, but safe)
        // Observable state unchanged (crash-safe)
        return BlockConnectionResult::Fail(
            "Failed to commit database batch",
            ConnectFailReason::DB_COMMIT
        );
    }

    // PHASE D: Apply Buffered UTXO Mutations (After Successful Commit)
    // ================================================================

    // Now that DB commit succeeded, apply the buffered mutations
    for (const auto& outpoint : utxos_to_remove) {
        utxo_view.removeUTXO(outpoint);
    }

    for (const auto& [outpoint, txout] : utxos_to_add) {
        utxo_view.addUTXO(outpoint, txout);
    }

    // Mark block as connected
    block_index_db.markBlockConnected(block_hash, true);

    // SUCCESS: Block fully connected
    return BlockConnectionResult::Ok(
        undo_file_id,
        undo_offset,
        undo_length,
        undo_checksum
    );
}

//=============================================================================
// DisconnectBlock Implementation
//=============================================================================

BlockDisconnectionResult DisconnectBlock(
    const Block& block,
    uint32_t height,
    IUTXOView& utxo_view,
    IBlockIndexDB& block_index_db,
    IUndoStorage& undo_storage,
    uint32_t undo_file_id,
    uint64_t undo_file_offset,
    uint64_t undo_length,
    uint32_t undo_checksum
) {
    // Calculate block hash
    Hash256 block_hash = generateFakeTestBlockId(block, height);

    // PRECONDITION CHECK: Block must be connected and have undo data
    if (!undo_storage.hasUndo(block_hash)) {
        return BlockDisconnectionResult::Fail("Undo data not found");
    }

    // PHASE A: Load Undo Data
    // =======================

    std::vector<uint8_t> undo_data;
    bool undo_read_ok = undo_storage.readUndo(
        block_hash,
        undo_file_id,
        undo_file_offset,
        undo_length,
        undo_checksum,
        undo_data
    );

    if (!undo_read_ok) {
        return BlockDisconnectionResult::Fail("Failed to read undo data or checksum mismatch");
    }

    // Deserialize undo data
    std::vector<TxOut> spent_utxos = deserializeUndoData(undo_data);

    // PHASE B: Reverse UTXO Mutations
    // ===============================

    // Remove outputs added by this block
    for (size_t tx_idx = 0; tx_idx < block.transactions.size(); tx_idx++) {
        const Transaction& tx = block.transactions[tx_idx];

        for (size_t out_idx = 0; out_idx < tx.outputs.size(); out_idx++) {
            OutPoint outpoint;
            // Convert Hash256 to uint256, then wrap in TxId - Phase M.4.3-B
            uint256 txid_raw;
            std::memcpy(txid_raw.data, block_hash.data.data(), 32);
            outpoint.txid = TxId(txid_raw);
            outpoint.vout = out_idx;

            utxo_view.removeUTXO(outpoint);
        }
    }

    // Restore inputs spent by this block (from undo data)
    size_t undo_idx = 0;
    for (size_t tx_idx = 1; tx_idx < block.transactions.size(); tx_idx++) {  // Skip coinbase
        const Transaction& tx = block.transactions[tx_idx];

        for (const auto& input : tx.inputs) {
            if (undo_idx >= spent_utxos.size()) {
                return BlockDisconnectionResult::Fail("Undo data corruption: not enough UTXOs");
            }

            // Restore the spent UTXO
            utxo_view.addUTXO(input.prevout, spent_utxos[undo_idx]);
            undo_idx++;
        }
    }

    // PHASE C: Database Commit
    // ========================

    bool db_commit_ok = block_index_db.commitBatch();

    if (!db_commit_ok) {
        return BlockDisconnectionResult::Fail("Failed to commit database batch");
    }

    // Mark block as no longer connected
    block_index_db.markBlockConnected(block_hash, false);

    // SUCCESS: Block disconnected, undo data preserved (reusable)
    return BlockDisconnectionResult::Ok();
}

} // namespace p2p
} // namespace dinero
