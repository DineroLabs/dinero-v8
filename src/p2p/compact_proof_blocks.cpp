#include "p2p/compact_proof_blocks.h"
#include "common/logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace p2p {

// ═══════════════════════════════════════════════════════════════════════════
// CompactProofBlock Serialization
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> CompactProofBlock::serialize() const {
    std::vector<uint8_t> data;

    // Serialize compact block first
    auto compact_data = compact_block.serialize();

    // Write compact block size (for deserialization boundary)
    uint32_t compact_size = static_cast<uint32_t>(compact_data.size());
    data.push_back(compact_size & 0xFF);
    data.push_back((compact_size >> 8) & 0xFF);
    data.push_back((compact_size >> 16) & 0xFF);
    data.push_back((compact_size >> 24) & 0xFF);

    // Append compact block data
    data.insert(data.end(), compact_data.begin(), compact_data.end());

    // Serialize and append proofs
    auto proof_data = utreexo_proofs.Serialize();
    data.insert(data.end(), proof_data.begin(), proof_data.end());

    return data;
}

CompactProofBlock CompactProofBlock::deserialize(const std::vector<uint8_t>& data) {
    CompactProofBlock result;

    if (data.size() < 4) {
        return result;  // Invalid data
    }

    size_t pos = 0;

    // Read compact block size
    uint32_t compact_size = data[pos] | (data[pos+1] << 8) |
                            (data[pos+2] << 16) | (data[pos+3] << 24);
    pos += 4;

    if (pos + compact_size > data.size()) {
        return result;  // Not enough data
    }

    // Deserialize compact block
    std::vector<uint8_t> compact_data(data.begin() + pos, data.begin() + pos + compact_size);
    result.compact_block = CompactBlock::deserialize(compact_data);
    pos += compact_size;

    // Deserialize proofs (remaining data)
    if (pos < data.size()) {
        std::vector<uint8_t> proof_data(data.begin() + pos, data.end());
        consensus::BlockUtreexoProofs::Deserialize(proof_data, result.utreexo_proofs);
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockTransactionsWithProofs Serialization
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> BlockTransactionsWithProofs::serialize() const {
    std::vector<uint8_t> data;

    // Block hash (32 bytes from hex)
    if (block_hash.size() == 64) {
        for (size_t i = 0; i < 32; ++i) {
            std::string byte_hex = block_hash.substr(i * 2, 2);
            data.push_back(static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16)));
        }
    } else {
        // Pad with zeros if invalid
        data.resize(data.size() + 32, 0);
    }

    // Transaction count (CompactSize)
    uint64_t tx_count = transactions.size();
    if (tx_count < 0xFD) {
        data.push_back(static_cast<uint8_t>(tx_count));
    } else if (tx_count <= 0xFFFF) {
        data.push_back(0xFD);
        data.push_back(tx_count & 0xFF);
        data.push_back((tx_count >> 8) & 0xFF);
    } else if (tx_count <= 0xFFFFFFFF) {
        data.push_back(0xFE);
        data.push_back(tx_count & 0xFF);
        data.push_back((tx_count >> 8) & 0xFF);
        data.push_back((tx_count >> 16) & 0xFF);
        data.push_back((tx_count >> 24) & 0xFF);
    } else {
        data.push_back(0xFF);
        for (int i = 0; i < 8; ++i) {
            data.push_back((tx_count >> (i * 8)) & 0xFF);
        }
    }

    // Transactions
    for (const auto& tx : transactions) {
        std::vector<uint8_t> tx_bytes = tx.Serialize();
        // Write tx size
        uint32_t tx_size = static_cast<uint32_t>(tx_bytes.size());
        if (tx_size < 0xFD) {
            data.push_back(static_cast<uint8_t>(tx_size));
        } else {
            data.push_back(0xFD);
            data.push_back(tx_size & 0xFF);
            data.push_back((tx_size >> 8) & 0xFF);
        }

        // Write tx data
        data.insert(data.end(), tx_bytes.begin(), tx_bytes.end());
    }

    // Proofs
    auto proof_data = proofs.Serialize();
    data.insert(data.end(), proof_data.begin(), proof_data.end());

    return data;
}

BlockTransactionsWithProofs BlockTransactionsWithProofs::deserialize(const std::vector<uint8_t>& data) {
    BlockTransactionsWithProofs result;

    if (data.size() < 33) {
        return result;  // Not enough data for hash + count
    }

    size_t pos = 0;

    // Block hash (32 bytes)
    std::stringstream ss;
    for (size_t i = 0; i < 32; ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(data[pos + i]);
    }
    result.block_hash = ss.str();
    pos += 32;

    // Transaction count (CompactSize)
    uint64_t tx_count = 0;
    if (data[pos] < 0xFD) {
        tx_count = data[pos];
        pos += 1;
    } else if (data[pos] == 0xFD && pos + 3 <= data.size()) {
        tx_count = data[pos + 1] | (data[pos + 2] << 8);
        pos += 3;
    } else if (data[pos] == 0xFE && pos + 5 <= data.size()) {
        tx_count = data[pos + 1] | (data[pos + 2] << 8) |
                   (data[pos + 3] << 16) | (data[pos + 4] << 24);
        pos += 5;
    } else if (data[pos] == 0xFF && pos + 9 <= data.size()) {
        tx_count = 0;
        for (int i = 0; i < 8; ++i) {
            tx_count |= static_cast<uint64_t>(data[pos + 1 + i]) << (i * 8);
        }
        pos += 9;
    }

    // Transactions (simplified - real implementation would use proper tx parsing)
    for (uint64_t i = 0; i < tx_count && pos < data.size(); ++i) {
        // Read tx size
        uint32_t tx_size = 0;
        if (data[pos] < 0xFD) {
            tx_size = data[pos];
            pos += 1;
        } else if (data[pos] == 0xFD && pos + 3 <= data.size()) {
            tx_size = data[pos + 1] | (data[pos + 2] << 8);
            pos += 3;
        }

        if (pos + tx_size > data.size()) break;

        // Read tx data as bytes
        std::vector<uint8_t> tx_data(data.begin() + pos, data.begin() + pos + tx_size);
        pos += tx_size;

        // Deserialize transaction using TransactionSerializer
        Transaction tx;
        if (TransactionSerializer::Deserialize(tx, tx_data)) {
            result.transactions.push_back(tx);
        }
    }

    // Remaining data is proofs
    if (pos < data.size()) {
        std::vector<uint8_t> proof_data(data.begin() + pos, data.end());
        consensus::BlockUtreexoProofs::Deserialize(proof_data, result.proofs);
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// CompactProofBlockManager Implementation
// ═══════════════════════════════════════════════════════════════════════════

CompactProofBlockManager::CompactProofBlockManager() {
    g_logger.info("Phase 34.7: CompactProofBlockManager initialized");
}

CompactProofBlockManager::~CompactProofBlockManager() = default;

bool CompactProofBlockManager::processCompactProofBlock(
    const std::string& peer_id,
    const CompactBlock& compact_block,
    const consensus::BlockUtreexoProofs& proofs) {

    std::lock_guard<std::mutex> lock(mutex_);

    std::string block_hash = compact_block.getHash();
    g_logger.debug("Phase 34.7: Processing compact-proof block " + block_hash.substr(0, 16) +
                   "... from " + peer_id + " (proofs: " + std::to_string(proofs.size()) + ")");

    stats_.compact_proof_blocks_received++;

    // Cache proofs if provided
    if (!proofs.empty() && proof_caching_) {
        cacheProofs(block_hash, proofs);
    }

    // If we're in stateless mode and have no proofs, request them
    if (stateless_mode_ && proofs.empty() && !hasProofs(block_hash)) {
        g_logger.debug("Phase 34.7: Stateless mode requires proofs for block " + block_hash.substr(0, 16));

        // Queue block pending proofs
        pending_proof_blocks_[block_hash] = compact_block;

        // Request proofs from peer
        if (request_proofs_callback_) {
            request_proofs_callback_(peer_id, block_hash);
            stats_.proof_requests_sent++;
        }

        return true;  // Queued, not yet reconstructed
    }

    // If we have CompactBlockManager, delegate transaction matching
    if (compact_block_manager_) {
        return compact_block_manager_->processCompactBlock(peer_id, compact_block);
    }

    return true;
}

bool CompactProofBlockManager::processBlockTxnProofs(
    const std::string& peer_id,
    const BlockTransactionsWithProofs& response,
    dinero::Block& full_block_out) {

    std::lock_guard<std::mutex> lock(mutex_);

    std::string block_hash = response.block_hash;
    g_logger.debug("Phase 34.7: Processing blocktxnproofs for " + block_hash.substr(0, 16) +
                   "... (txs: " + std::to_string(response.transactions.size()) +
                   ", proofs: " + std::to_string(response.proofs.size()) + ")");

    stats_.blocktxnproofs_received++;

    // Cache the proofs
    if (!response.proofs.empty() && proof_caching_) {
        cacheProofs(block_hash, response.proofs);
    }

    // Check if we have a pending compact block for this
    auto pending_it = pending_proof_blocks_.find(block_hash);
    if (pending_it != pending_proof_blocks_.end()) {
        // Reconstruct with CompactBlockManager if available
        if (compact_block_manager_) {
            BlockTransactions bt;
            bt.block_hash = block_hash;
            bt.transactions = response.transactions;

            if (compact_block_manager_->processMissingTransactions(bt, full_block_out)) {
                pending_proof_blocks_.erase(pending_it);
                stats_.blocks_reconstructed_with_proofs++;
                g_logger.info("Phase 34.7: Block " + block_hash.substr(0, 16) +
                             "... reconstructed with " + std::to_string(response.proofs.size()) + " proofs");
                return true;
            }
        }
    }

    return false;
}

bool CompactProofBlockManager::reconstructBlockWithProofs(
    const CompactBlock& compact_block,
    const consensus::BlockUtreexoProofs& proofs,
    dinero::Block& full_block_out) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (!compact_block_manager_) {
        g_logger.warning("Phase 34.7: Cannot reconstruct - no CompactBlockManager");
        return false;
    }

    // First, reconstruct the block using CompactBlockManager
    if (!compact_block_manager_->reconstructFullBlock(compact_block, full_block_out)) {
        g_logger.debug("Phase 34.7: Standard reconstruction failed, may need missing txs");
        return false;
    }

    // Cache proofs for this block
    std::string block_hash = compact_block.getHash();
    if (!proofs.empty() && proof_caching_) {
        cacheProofs(block_hash, proofs);
    }

    stats_.blocks_reconstructed_with_proofs++;
    g_logger.info("Phase 34.7: Block " + block_hash.substr(0, 16) +
                 "... reconstructed with proofs (size: " + std::to_string(proofs.size()) + ")");

    return true;
}

BlockTransactionsRequest CompactProofBlockManager::createMissingTxProofRequest(
    const CompactBlock& compact_block) {

    // Use CompactBlockManager to create the request if available
    if (compact_block_manager_) {
        return compact_block_manager_->createMissingTxRequest(compact_block);
    }

    // Fallback: create basic request
    BlockTransactionsRequest request;
    request.block_hash = compact_block.getHash();
    return request;
}

void CompactProofBlockManager::cacheProofs(const std::string& block_hash,
                                            const consensus::BlockUtreexoProofs& proofs) {
    // Thread safety handled by caller

    // Check if already cached
    if (proof_cache_.find(block_hash) != proof_cache_.end()) {
        updateCacheOrder(block_hash);
        stats_.proof_cache_hits++;
        return;
    }

    // Evict if at capacity
    if (proof_cache_.size() >= max_proof_cache_size_) {
        evictOldestProofs();
    }

    // Add to cache
    proof_cache_[block_hash] = proofs;
    proof_cache_order_.push_back(block_hash);

    g_logger.debug("Phase 34.7: Cached proofs for block " + block_hash.substr(0, 16) +
                  "... (cache size: " + std::to_string(proof_cache_.size()) + ")");
}

std::optional<consensus::BlockUtreexoProofs> CompactProofBlockManager::getCachedProofs(
    const std::string& block_hash) const {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = proof_cache_.find(block_hash);
    if (it != proof_cache_.end()) {
        stats_.proof_cache_hits++;
        return it->second;
    }

    stats_.proof_cache_misses++;
    return std::nullopt;
}

bool CompactProofBlockManager::hasProofs(const std::string& block_hash) const {
    // No lock - called from locked context
    return proof_cache_.find(block_hash) != proof_cache_.end();
}

void CompactProofBlockManager::removeProofs(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    proof_cache_.erase(block_hash);

    auto it = std::find(proof_cache_order_.begin(), proof_cache_order_.end(), block_hash);
    if (it != proof_cache_order_.end()) {
        proof_cache_order_.erase(it);
    }
}

BlockTransactionsWithProofs CompactProofBlockManager::createBlockTxnProofsResponse(
    const BlockTransactionsRequest& request,
    const dinero::Block& block,
    const consensus::BlockUtreexoProofs& proofs) {

    BlockTransactionsWithProofs response;
    response.block_hash = request.block_hash;
    response.proofs = proofs;

    // Extract requested transactions by index
    for (uint32_t idx : request.indexes) {
        if (idx < block.vtx.size()) {
            response.transactions.push_back(block.vtx[idx]);
        }
    }

    stats_.blocktxnproofs_sent++;

    g_logger.debug("Phase 34.7: Created blocktxnproofs response for " + request.block_hash.substr(0, 16) +
                  "... (txs: " + std::to_string(response.transactions.size()) +
                  ", proofs: " + std::to_string(proofs.size()) + ")");

    return response;
}

void CompactProofBlockManager::evictOldestProofs() {
    // Thread safety handled by caller

    if (proof_cache_order_.empty()) return;

    // Remove oldest entry (LRU)
    std::string oldest = proof_cache_order_.front();
    proof_cache_order_.erase(proof_cache_order_.begin());
    proof_cache_.erase(oldest);

    g_logger.debug("Phase 34.7: Evicted proofs for block " + oldest.substr(0, 16) + "...");
}

void CompactProofBlockManager::updateCacheOrder(const std::string& block_hash) {
    // Move to end of order (most recently used)
    auto it = std::find(proof_cache_order_.begin(), proof_cache_order_.end(), block_hash);
    if (it != proof_cache_order_.end()) {
        proof_cache_order_.erase(it);
        proof_cache_order_.push_back(block_hash);
    }
}

CompactProofBlockManager::Stats CompactProofBlockManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.current_cache_size = proof_cache_.size();
    return s;
}

std::string CompactProofBlockManager::getStatsString() const {
    Stats s = getStats();
    std::stringstream ss;
    ss << "Phase 34.7 Compact-Proof Blocks:\n"
       << "  Compact-proof blocks received: " << s.compact_proof_blocks_received << "\n"
       << "  Blocks reconstructed with proofs: " << s.blocks_reconstructed_with_proofs << "\n"
       << "  Proof requests sent: " << s.proof_requests_sent << "\n"
       << "  BlockTxnProofs received: " << s.blocktxnproofs_received << "\n"
       << "  BlockTxnProofs sent: " << s.blocktxnproofs_sent << "\n"
       << "  Proof cache hits: " << s.proof_cache_hits << "\n"
       << "  Proof cache misses: " << s.proof_cache_misses << "\n"
       << "  Current cache size: " << s.current_cache_size << " blocks\n"
       << "  Bandwidth saved: " << s.bandwidth_saved_bytes << " bytes";
    return ss.str();
}

} // namespace p2p
} // namespace dinero
