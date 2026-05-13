// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/compact_block.h"
#include "daemon/mempool.h"
#include "p2p/block_download_scheduler.h"
#include "crypto/sha256.h"
#include "crypto/siphash.h"
#include "util/ser.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <unordered_set>

namespace dinero {

// ============================================================================
// Serialization
// ============================================================================

std::vector<uint8_t> PrefilledTransaction::Serialize() const {
    std::vector<uint8_t> result;
    // Index (CompactSize)
    ser::writeCompactSize(index, result);

    // Transaction bytes (no length prefix; self-delimiting)
    std::vector<uint8_t> tx_bytes = tx.Serialize(TxSerializationMode::WithWitness);
    result.insert(result.end(), tx_bytes.begin(), tx_bytes.end());
    return result;
}

PrefilledTransaction PrefilledTransaction::Deserialize(const std::vector<uint8_t>& data, size_t& offset) {
    PrefilledTransaction result;
    if (offset >= data.size()) {
        return result;
    }

    ser::VarIntDecode idx_dec;
    if (!ser::readCompactSize(data.data() + offset, data.size() - offset, idx_dec)) {
        return result;
    }
    result.index = static_cast<uint32_t>(idx_dec.value);
    offset += idx_dec.consumed;

    if (offset >= data.size()) {
        return result;
    }

    std::vector<uint8_t> remaining(data.begin() + offset, data.end());
    Transaction tx;
    size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(tx, remaining, consumed) || consumed == 0) {
        return result;
    }
    result.tx = tx;
    offset += consumed;
    return result;
}

std::vector<uint8_t> CompactBlock::Serialize() const {
    std::vector<uint8_t> result;
    // Header (128 bytes, canonical)
    std::string header_bytes = header.Serialize();
    result.insert(result.end(), header_bytes.begin(), header_bytes.end());

    // Nonce (8 bytes, LE)
    ser::writeLE<uint64_t>(nonce, result);

    // Short txids
    ser::writeCompactSize(short_txids.size(), result);
    for (uint64_t short_id : short_txids) {
        // 6-byte little-endian
        for (int i = 0; i < 6; ++i) {
            result.push_back(static_cast<uint8_t>((short_id >> (8 * i)) & 0xFF));
        }
    }

    // Prefilled transactions
    ser::writeCompactSize(prefilled.size(), result);
    for (const auto& p : prefilled) {
        std::vector<uint8_t> p_bytes = p.Serialize();
        result.insert(result.end(), p_bytes.begin(), p_bytes.end());
    }
    return result;
}

CompactBlock CompactBlock::Deserialize(const std::vector<uint8_t>& data) {
    CompactBlock result;
    const size_t header_size = sizeof(BlockHeader);
    if (data.size() < header_size + 8) {
        return result;
    }

    // Header
    auto header_opt = BlockHeader::Deserialize(data.data(), data.size());
    if (!header_opt.has_value()) {
        return result;
    }
    result.header = *header_opt;

    size_t offset = header_size;

    // Nonce
    if (offset + sizeof(uint64_t) > data.size()) {
        return result;
    }
    uint64_t nonce_val = 0;
    if (!ser::readLE<uint64_t>(data.data() + offset, data.size() - offset, nonce_val)) {
        return result;
    }
    result.nonce = nonce_val;
    offset += sizeof(uint64_t);

    // Short txids
    ser::VarIntDecode count_dec;
    if (!ser::readCompactSize(data.data() + offset, data.size() - offset, count_dec)) {
        return result;
    }
    uint64_t short_count = count_dec.value;
    offset += count_dec.consumed;

    // Sanity check: each short txid is 6 bytes, so count can't exceed remaining data / 6
    // Also apply a hard cap to prevent allocation attacks
    constexpr uint64_t MAX_SHORT_TXIDS = 100000;  // ~600KB max
    if (short_count > MAX_SHORT_TXIDS || short_count * 6 > data.size() - offset) {
        return result;
    }

    result.short_txids.clear();
    result.short_txids.reserve(static_cast<size_t>(short_count));
    for (uint64_t i = 0; i < short_count; ++i) {
        if (offset + 6 > data.size()) {
            return result;
        }
        uint64_t short_id = 0;
        for (int b = 0; b < 6; ++b) {
            short_id |= (static_cast<uint64_t>(data[offset + b]) << (8 * b));
        }
        offset += 6;
        result.short_txids.push_back(short_id);
    }

    // Prefilled transactions
    if (!ser::readCompactSize(data.data() + offset, data.size() - offset, count_dec)) {
        return result;
    }
    uint64_t prefilled_count = count_dec.value;
    offset += count_dec.consumed;

    // Sanity check: apply hard cap to prevent allocation attacks
    // Prefilled txs are variable size but must have at least 1 byte each
    constexpr uint64_t MAX_PREFILLED = 10000;
    if (prefilled_count > MAX_PREFILLED || prefilled_count > data.size() - offset) {
        return result;
    }

    result.prefilled.clear();
    result.prefilled.reserve(static_cast<size_t>(prefilled_count));
    for (uint64_t i = 0; i < prefilled_count; ++i) {
        size_t before = offset;
        PrefilledTransaction p = PrefilledTransaction::Deserialize(data, offset);
        if (offset == before) {
            return result;
        }
        result.prefilled.push_back(p);
    }

    return result;
}

std::vector<uint8_t> BlockTransactionsRequest::Serialize() const {
    std::vector<uint8_t> result;
    // Block hash (32 bytes)
    result.insert(result.end(), block_hash.data, block_hash.data + 32);

    // Index count
    ser::writeCompactSize(indexes.size(), result);

    for (uint32_t idx : indexes) {
        ser::writeCompactSize(idx, result);
    }
    return result;
}

BlockTransactionsRequest BlockTransactionsRequest::Deserialize(const std::vector<uint8_t>& data) {
    BlockTransactionsRequest result;
    if (data.size() < 32) {
        return result;
    }

    std::memcpy(result.block_hash.data, data.data(), 32);
    size_t offset = 32;

    ser::VarIntDecode count_dec;
    if (!ser::readCompactSize(data.data() + offset, data.size() - offset, count_dec)) {
        return result;
    }
    uint64_t count = count_dec.value;
    offset += count_dec.consumed;

    // Sanity check: each index is at least 1 byte (CompactSize), apply hard cap
    constexpr uint64_t MAX_INDEXES = 100000;
    if (count > MAX_INDEXES || count > data.size() - offset) {
        return result;
    }

    result.indexes.clear();
    result.indexes.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        ser::VarIntDecode idx_dec;
        if (!ser::readCompactSize(data.data() + offset, data.size() - offset, idx_dec)) {
            return result;
        }
        result.indexes.push_back(static_cast<uint32_t>(idx_dec.value));
        offset += idx_dec.consumed;
    }
    return result;
}

std::vector<uint8_t> BlockTransactions::Serialize() const {
    std::vector<uint8_t> result;
    // Block hash (32 bytes)
    result.insert(result.end(), block_hash.data, block_hash.data + 32);

    // Transaction count
    ser::writeCompactSize(transactions.size(), result);

    for (const auto& tx : transactions) {
        std::vector<uint8_t> tx_bytes = tx.Serialize(TxSerializationMode::WithWitness);
        result.insert(result.end(), tx_bytes.begin(), tx_bytes.end());
    }
    return result;
}

BlockTransactions BlockTransactions::Deserialize(const std::vector<uint8_t>& data) {
    BlockTransactions result;
    if (data.size() < 32) {
        return result;
    }

    std::memcpy(result.block_hash.data, data.data(), 32);
    size_t offset = 32;

    ser::VarIntDecode count_dec;
    if (!ser::readCompactSize(data.data() + offset, data.size() - offset, count_dec)) {
        return result;
    }
    uint64_t count = count_dec.value;
    offset += count_dec.consumed;

    // Sanity check: each transaction is at least 10 bytes, apply hard cap
    constexpr uint64_t MAX_TRANSACTIONS = 100000;
    if (count > MAX_TRANSACTIONS || count > (data.size() - offset) / 10) {
        return result;
    }

    result.transactions.clear();
    result.transactions.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count && offset < data.size(); ++i) {
        std::vector<uint8_t> remaining(data.begin() + offset, data.end());
        Transaction tx;
        size_t consumed = 0;
        if (!TransactionSerializer::Deserialize(tx, remaining, consumed) || consumed == 0) {
            return result;
        }
        result.transactions.push_back(tx);
        offset += consumed;
    }
    return result;
}

// ============================================================================
// SipHash-2-4 — delegates to shared crypto::SipHash24
// ============================================================================

uint64_t CompactBlockCodec::SipHash24(uint64_t k0, uint64_t k1, const uint256& data) {
    return crypto::SipHash24(k0, k1, data.data, 32);
}

uint64_t CompactBlockCodec::GenerateNonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    return dist(gen);
}

// ============================================================================
// Short Transaction ID
// ============================================================================

uint64_t CompactBlockCodec::ComputeShortTxId(
    const uint256& block_hash,
    uint64_t nonce,
    const uint256& txid
) {
    // Extract k0 from block hash (first 8 bytes)
    uint64_t k0;
    std::memcpy(&k0, block_hash.data, sizeof(k0));

    // Use nonce as k1
    uint64_t k1 = nonce;

    // Compute SipHash-2-4
    uint64_t hash = SipHash24(k0, k1, txid);

    // Return 48-bit short txid (mask upper 16 bits)
    return hash & 0xFFFFFFFFFFFFULL;
}

// ============================================================================
// Compact Block Creation
// ============================================================================

CompactBlock CompactBlockCodec::CreateCompactBlock(const Block& block) {
    CompactBlock compact;

    // Copy header
    compact.header = block.header;

    // Generate random nonce for collision resistance
    compact.nonce = GenerateNonce();

    // Get block hash for short txid computation
    uint256 block_hash = block.header.GetHash();

    // Always prefill coinbase (index 0)
    if (!block.vtx.empty()) {
        compact.prefilled.emplace_back(0, block.vtx[0]);
    }

    // Generate short txids for remaining transactions
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        TxId txid = block.vtx[i].GetTxid();  // Phase M.4: GetTxid() returns TxId
        uint64_t short_txid = ComputeShortTxId(block_hash, compact.nonce, txid.AsUint256());
        compact.short_txids.push_back(short_txid);
    }

    return compact;
}

// ============================================================================
// Block Reconstruction
// ============================================================================

std::optional<Block> CompactBlockCodec::ReconstructBlock(
    const CompactBlock& compact,
    const Mempool* mempool,
    std::vector<uint32_t>& out_missing_indexes
) {
    Block reconstructed;
    if (!ReconstructPartialBlock(compact, mempool, reconstructed, out_missing_indexes)) {
        return std::nullopt;
    }

    if (!out_missing_indexes.empty()) {
        return std::nullopt;
    }

    return reconstructed;
}

bool CompactBlockCodec::ReconstructPartialBlock(
    const CompactBlock& compact,
    const Mempool* mempool,
    Block& out_partial_block,
    std::vector<uint32_t>& out_missing_indexes
) {
    if (!mempool) {
        return false;
    }

    // Calculate total transaction count
    size_t tx_count = compact.short_txids.size() + compact.prefilled.size();
    out_partial_block = Block{};
    out_partial_block.header = compact.header;
    out_partial_block.vtx.resize(tx_count);

    // Track which indexes are filled
    std::vector<bool> filled(tx_count, false);

    // Place prefilled transactions at correct indexes
    for (const auto& prefilled : compact.prefilled) {
        if (prefilled.index >= tx_count) {
            return false;  // Invalid index
        }
        out_partial_block.vtx[prefilled.index] = prefilled.tx;
        filled[prefilled.index] = true;
    }

    // Get block hash for short txid computation
    uint256 block_hash = compact.header.GetHash();

    // Build map of short txid → transaction from mempool
    std::unordered_map<uint64_t, Transaction> short_to_tx;
    std::unordered_set<uint64_t> collisions;

    mempool->forEachEntry([&](const MempoolEntry& entry) {
        const Transaction& tx = entry.tx;
        TxId txid = tx.GetTxid();
        uint64_t short_txid = ComputeShortTxId(block_hash, compact.nonce, txid.AsUint256());

        auto [it, inserted] = short_to_tx.emplace(short_txid, tx);
        if (!inserted) {
            // Collision: mark and remove ambiguous entry
            collisions.insert(short_txid);
            short_to_tx.erase(it);
        }
    });

    // Match short txids to transactions
    uint32_t next_unfilled_index = 0;
    out_missing_indexes.clear();

    for (size_t i = 0; i < compact.short_txids.size(); ++i) {
        // Find next unfilled index
        while (next_unfilled_index < tx_count && filled[next_unfilled_index]) {
            ++next_unfilled_index;
        }

        if (next_unfilled_index >= tx_count) {
            return false;  // Something wrong with indexing
        }

        uint64_t short_txid = compact.short_txids[i];

        // Try to find transaction in mempool
        if (collisions.find(short_txid) != collisions.end()) {
            out_missing_indexes.push_back(next_unfilled_index);
        } else {
            auto it = short_to_tx.find(short_txid);
            if (it == short_to_tx.end()) {
                out_missing_indexes.push_back(next_unfilled_index);
            } else {
                out_partial_block.vtx[next_unfilled_index] = it->second;
                filled[next_unfilled_index] = true;
            }
        }

        ++next_unfilled_index;
    }
    return true;
}

std::optional<Block> CompactBlockCodec::CompleteReconstruction(
    const Block& partial_block,
    const std::vector<Transaction>& missing_txs,
    const std::vector<uint32_t>& missing_indexes
) {
    if (missing_txs.size() != missing_indexes.size()) {
        return std::nullopt;  // Size mismatch
    }

    Block reconstructed = partial_block;
    std::vector<bool> filled(reconstructed.vtx.size(), false);

    for (size_t i = 0; i < reconstructed.vtx.size(); ++i) {
        if (!reconstructed.vtx[i].vin.empty() || !reconstructed.vtx[i].vout.empty()) {
            filled[i] = true;
        }
    }

    // Place missing transactions at specified indexes
    for (size_t i = 0; i < missing_indexes.size(); ++i) {
        uint32_t index = missing_indexes[i];
        if (index >= reconstructed.vtx.size()) {
            return std::nullopt;
        }
        reconstructed.vtx[index] = missing_txs[i];
        filled[index] = true;
    }

    // Verify all slots filled
    for (bool slot_filled : filled) {
        if (!slot_filled) {
            return std::nullopt;  // Incomplete block
        }
    }

    return reconstructed;
}

// ============================================================================
// Peer Selection Strategy
// ============================================================================

bool CompactBlockStrategy::ShouldSendCompactBlock(double peer_score, SyncPhase sync_phase) {
    // IBD: Always full blocks (parallel download, no round trips)
    if (sync_phase == SyncPhase::IBD) {
        return false;
    }

    // Steady-state: Compact for good peers (score > 70)
    if (sync_phase == SyncPhase::STEADY_STATE && peer_score > 70.0) {
        return true;
    }

    // Catching up: Compact only for excellent peers (score > 85)
    if (sync_phase == SyncPhase::CATCHING_UP && peer_score > 85.0) {
        return true;
    }

    // Default: full blocks
    return false;
}

bool CompactBlockStrategy::ShouldRequestCompactBlock(double peer_score, SyncPhase sync_phase) {
    // Same logic as sending
    return ShouldSendCompactBlock(peer_score, sync_phase);
}

// ============================================================================
// Phase G.16: Adaptive Compact Block Strategy
// ============================================================================

bool AdaptiveCompactBlockStrategy::ShouldSendCompactBlock(double peer_score, SyncPhase sync_phase) const {
    // IBD: Always full blocks (parallel download, no round trips)
    if (sync_phase == SyncPhase::IBD) {
        return false;
    }

    // Steady-state: Compact for peers above adaptive threshold
    if (sync_phase == SyncPhase::STEADY_STATE && peer_score > steady_state_threshold_) {
        return true;
    }

    // Catching up: Compact only for peers above adaptive threshold
    if (sync_phase == SyncPhase::CATCHING_UP && peer_score > catching_up_threshold_) {
        return true;
    }

    // Default: full blocks
    return false;
}

bool AdaptiveCompactBlockStrategy::ShouldRequestCompactBlock(double peer_score, SyncPhase sync_phase) const {
    // Same logic as sending
    return ShouldSendCompactBlock(peer_score, sync_phase);
}

} // namespace dinero
