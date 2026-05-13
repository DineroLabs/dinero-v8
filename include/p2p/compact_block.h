// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Phase G.13: Compact Block Relay
 *
 * Bandwidth optimization using:
 * - Short transaction IDs (6 bytes vs 32 bytes)
 * - Block reconstruction from mempool
 * - Intelligent peer selection (G.10 scoring + G.12 sync phase)
 *
 * Protocol:
 * 1. Sender: cmpctblock (header + short txids + prefilled txs)
 * 2. Receiver: Reconstruct from mempool
 * 3. If missing txs: getblocktxn (request missing by index)
 * 4. Sender: blocktxn (respond with missing txs)
 *
 * Bandwidth savings: ~95% (1MB block → ~50KB compact block)
 *
 * Integration:
 * - G.10: Peer scoring determines who gets compact blocks
 * - G.12: Sync phase determines compact vs full strategy
 * - IBD: Full blocks (parallel download, no round trips)
 * - Steady-state: Compact blocks for high-scoring peers
 */

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include <vector>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace dinero {

// Forward declarations
class Mempool;
enum class SyncPhase;  // From block_download_scheduler.h

/**
 * Prefilled transaction (always included in compact block)
 * Used for coinbase and transactions unlikely to be in mempool
 */
struct PrefilledTransaction {
    uint32_t index;         // Index in block
    Transaction tx;         // Full transaction

    PrefilledTransaction() : index(0) {}
    PrefilledTransaction(uint32_t idx, const Transaction& transaction)
        : index(idx), tx(transaction) {}

    // Serialization
    std::vector<uint8_t> Serialize() const;
    static PrefilledTransaction Deserialize(const std::vector<uint8_t>& data, size_t& offset);
};

/**
 * Compact block representation
 * Header + short txids + prefilled transactions
 */
struct CompactBlock {
    BlockHeader header;                             // Full block header
    uint64_t nonce;                                 // Random nonce for short txid calculation
    std::vector<uint64_t> short_txids;              // 48-bit short transaction IDs
    std::vector<PrefilledTransaction> prefilled;    // Prefilled transactions (coinbase, etc)

    CompactBlock() : nonce(0) {}

    // Serialization
    std::vector<uint8_t> Serialize() const;
    static CompactBlock Deserialize(const std::vector<uint8_t>& data);

    // Get total transaction count (short txids + prefilled)
    size_t GetTxCount() const {
        return short_txids.size() + prefilled.size();
    }
};

/**
 * Request for missing transactions
 * Sent when compact block reconstruction fails
 */
struct BlockTransactionsRequest {
    uint256 block_hash;                 // Block hash being requested
    std::vector<uint32_t> indexes;      // Indexes of missing transactions

    BlockTransactionsRequest() = default;
    BlockTransactionsRequest(const uint256& hash, const std::vector<uint32_t>& idx)
        : block_hash(hash), indexes(idx) {}

    // Serialization
    std::vector<uint8_t> Serialize() const;
    static BlockTransactionsRequest Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Response with missing transactions
 */
struct BlockTransactions {
    uint256 block_hash;                         // Block hash
    std::vector<Transaction> transactions;      // Missing transactions

    BlockTransactions() = default;
    BlockTransactions(const uint256& hash, const std::vector<Transaction>& txs)
        : block_hash(hash), transactions(txs) {}

    // Serialization
    std::vector<uint8_t> Serialize() const;
    static BlockTransactions Deserialize(const std::vector<uint8_t>& data);
};

/**
 * CompactBlockCodec — Encoding/Decoding/Reconstruction
 *
 * Responsibilities:
 * 1. Create compact blocks from full blocks
 * 2. Compute short transaction IDs (SipHash-2-4)
 * 3. Reconstruct full blocks from compact blocks + mempool
 * 4. Handle missing transaction requests
 */
class CompactBlockCodec {
public:
    /**
     * Create compact block from full block
     *
     * Strategy:
     * - Always prefill coinbase (index 0)
     * - Use short txids for all other transactions
     * - Generate random nonce for collision resistance
     *
     * @param block Full block to encode
     * @return Compact block representation
     */
    static CompactBlock CreateCompactBlock(const Block& block);

    /**
     * Reconstruct full block from compact block + mempool
     *
     * Process:
     * 1. Place prefilled transactions at correct indexes
     * 2. For each short txid, search mempool for match
     * 3. If all found → return full block
     * 4. If missing → return nullopt (caller must request via getblocktxn)
     *
     * @param compact Compact block to reconstruct
     * @param mempool Mempool to search for transactions
     * @param out_missing_indexes Output: indexes of missing transactions (if any)
     * @return Full block if successful, nullopt if missing transactions
     */
    static std::optional<Block> ReconstructBlock(
        const CompactBlock& compact,
        const Mempool* mempool,
        std::vector<uint32_t>& out_missing_indexes
    );

    /**
     * Reconstruct as much of the block as possible from compact data + mempool.
     *
     * Unlike ReconstructBlock(), this always returns the partially-filled block
     * and the precise indexes that still need to be supplied via blocktxn.
     *
     * @param compact Compact block to reconstruct
     * @param mempool Mempool to search for transactions
     * @param out_partial_block Output: partially reconstructed block
     * @param out_missing_indexes Output: indexes of transactions still missing
     * @return true if the compact block structure was valid, false if malformed
     */
    static bool ReconstructPartialBlock(
        const CompactBlock& compact,
        const Mempool* mempool,
        Block& out_partial_block,
        std::vector<uint32_t>& out_missing_indexes
    );

    /**
     * Complete block reconstruction with missing transactions
     *
     * Called after receiving blocktxn response.
     * Combines partial reconstruction with missing transactions.
     *
     * @param compact Original compact block
     * @param missing_txs Missing transactions from blocktxn message
     * @param missing_indexes Indexes where missing txs belong
     * @return Full block
     */
    static std::optional<Block> CompleteReconstruction(
        const Block& partial_block,
        const std::vector<Transaction>& missing_txs,
        const std::vector<uint32_t>& missing_indexes
    );

    /**
     * Compute short transaction ID (48-bit SipHash-2-4)
     *
     * Formula: SipHash-2-4(k0=block_hash[0..7], k1=nonce, msg=txid) & 0xFFFFFFFFFFFF
     *
     * Collision resistance:
     * - 48 bits = 281 trillion combinations
     * - For 10,000 tx block: collision probability ~0.00002%
     *
     * @param block_hash Block hash (provides k0)
     * @param nonce Random nonce (provides k1)
     * @param txid Transaction hash
     * @return 48-bit short transaction ID
     */
    static uint64_t ComputeShortTxId(const uint256& block_hash, uint64_t nonce, const uint256& txid);

    /**
     * Get bandwidth savings estimate
     *
     * @param full_block_size Size of full block (bytes)
     * @param tx_count Number of transactions
     * @return Estimated compact block size (bytes)
     */
    static size_t EstimateCompactSize(size_t full_block_size, size_t tx_count) {
        // Header: 80 bytes
        // Nonce: 8 bytes
        // Short txids: 6 bytes each (48 bits)
        // Prefilled: ~200 bytes (coinbase)
        return 80 + 8 + (tx_count * 6) + 200;
    }

private:
    /**
     * SipHash-2-4 implementation
     * Cryptographic hash function optimized for short inputs
     */
    static uint64_t SipHash24(uint64_t k0, uint64_t k1, const uint256& data);

    /**
     * Generate random nonce for short txid calculation
     */
    static uint64_t GenerateNonce();
};

/**
 * Peer selection strategy for compact blocks
 * Integrates with G.10 peer scoring + G.12 sync phase
 */
struct CompactBlockStrategy {
    /**
     * Decide whether to send compact block to peer
     *
     * Strategy:
     * - IBD: Always full blocks (parallel download, no round trips)
     * - Catching up: Compact only for excellent peers (score > 85)
     * - Steady-state: Compact for good peers (score > 70)
     *
     * @param peer_score Peer performance score (0-100)
     * @param sync_phase Current sync phase
     * @return true if should send compact block
     */
    static bool ShouldSendCompactBlock(double peer_score, SyncPhase sync_phase);

    /**
     * Decide whether to request compact blocks from peer
     *
     * @param peer_score Peer performance score
     * @param sync_phase Current sync phase
     * @return true if should request compact blocks
     */
    static bool ShouldRequestCompactBlock(double peer_score, SyncPhase sync_phase);
};

/**
 * Phase G.16: Adaptive Compact Block Strategy
 *
 * Auto-adjusts score thresholds based on network conditions:
 * - Reconstruction success rate
 * - Round-trip efficiency
 * - Mempool synchronization quality
 *
 * Adaptive algorithm:
 * - High success (>90%) → Lower thresholds (more compact blocks)
 * - Medium success (70-90%) → Default thresholds
 * - Low success (<70%) → Higher thresholds (fewer compact blocks)
 */
class AdaptiveCompactBlockStrategy {
public:
    AdaptiveCompactBlockStrategy()
        : steady_state_threshold_(70.0)
        , catching_up_threshold_(85.0)
        , total_reconstructions_(0)
        , successful_reconstructions_(0)
        , total_round_trips_(0)
        , total_round_trip_ms_(0)
        , avg_round_trip_ms_(0.0) {}

    /**
     * Record compact block reconstruction attempt
     *
     * @param success Whether reconstruction succeeded
     * @param round_trip_ms Round-trip time for missing txs (0 if no round trip)
     */
    void RecordReconstruction(bool success, uint64_t round_trip_ms = 0) {
        total_reconstructions_++;
        if (success) {
            successful_reconstructions_++;
        }

        if (round_trip_ms > 0) {
            total_round_trips_++;
            total_round_trip_ms_ += round_trip_ms;
            // Simple average
            avg_round_trip_ms_ = static_cast<double>(total_round_trip_ms_) / total_round_trips_;
        }

        UpdateThresholds();
    }

    /**
     * Get current success rate (0.0 - 1.0)
     */
    double GetSuccessRate() const {
        if (total_reconstructions_ == 0) return 1.0;
        return static_cast<double>(successful_reconstructions_) / total_reconstructions_;
    }

    /**
     * Get adaptive threshold for steady-state mode
     */
    double GetSteadyStateThreshold() const {
        return steady_state_threshold_;
    }

    /**
     * Get adaptive threshold for catching-up mode
     */
    double GetCatchingUpThreshold() const {
        return catching_up_threshold_;
    }

    /**
     * Decide whether to send compact block (using adaptive thresholds)
     */
    bool ShouldSendCompactBlock(double peer_score, SyncPhase sync_phase) const;

    /**
     * Decide whether to request compact block (using adaptive thresholds)
     */
    bool ShouldRequestCompactBlock(double peer_score, SyncPhase sync_phase) const;

    /**
     * Reset statistics (for testing)
     */
    void Reset() {
        total_reconstructions_ = 0;
        successful_reconstructions_ = 0;
        total_round_trips_ = 0;
        total_round_trip_ms_ = 0;
        avg_round_trip_ms_ = 0.0;
        steady_state_threshold_ = 70.0;
        catching_up_threshold_ = 85.0;
    }

private:
    /**
     * Update thresholds based on reconstruction performance
     */
    void UpdateThresholds() {
        // Need minimum sample size for statistical significance
        if (total_reconstructions_ < 10) {
            return;  // Keep default thresholds
        }

        double success_rate = GetSuccessRate();

        // High success (>90%) → Lower thresholds (more compact blocks)
        if (success_rate > 0.90) {
            steady_state_threshold_ = 60.0;   // Lower from 70
            catching_up_threshold_ = 75.0;    // Lower from 85
        }
        // Medium success (70-90%) → Default thresholds
        else if (success_rate >= 0.70) {
            steady_state_threshold_ = 70.0;   // Default
            catching_up_threshold_ = 85.0;    // Default
        }
        // Low success (<70%) → Higher thresholds (fewer compact blocks)
        else {
            steady_state_threshold_ = 80.0;   // Higher from 70
            catching_up_threshold_ = 95.0;    // Higher from 85
        }

        // Additional penalty if round trips are slow (>500ms avg)
        if (total_round_trips_ > 0 && avg_round_trip_ms_ > 500.0) {
            steady_state_threshold_ += 5.0;
            catching_up_threshold_ += 5.0;
        }
    }

    double steady_state_threshold_;      // Adaptive threshold for steady-state
    double catching_up_threshold_;       // Adaptive threshold for catching-up
    uint64_t total_reconstructions_;     // Total reconstruction attempts
    uint64_t successful_reconstructions_; // Successful reconstructions
    uint64_t total_round_trips_;         // Number of round trips for missing txs
    uint64_t total_round_trip_ms_;       // Total round-trip time (milliseconds)
    double avg_round_trip_ms_;           // Average round-trip time (simple average)
};

/**
 * Phase G.17: Mempool Intelligence
 *
 * Optimizes compact block relay using mempool synchronization state:
 * - Mempool sync hints: Track which transactions each peer has
 * - Compact success prediction: Predict reconstruction success rates
 * - Fee-aware propagation: Prioritize high-fee transactions
 *
 * Benefits:
 * - Reduces failed reconstructions by predicting peer mempool state
 * - Improves bandwidth efficiency with intelligent peer selection
 * - Prioritizes valuable transactions for faster confirmation
 */

/**
 * Per-peer mempool synchronization tracker
 * Tracks which transactions we believe a peer has in their mempool
 */
class MempoolSyncTracker {
public:
    /**
     * Record that we sent a transaction to this peer
     */
    void RecordTxSent(const uint256& txid, uint64_t fee_rate, uint64_t timestamp_ms) {
        sent_txs_[txid] = TxInfo{fee_rate, timestamp_ms};

        // Cleanup old entries (older than 10 minutes)
        CleanupOldEntries(timestamp_ms);
    }

    /**
     * Record that we received a transaction from this peer
     */
    void RecordTxReceived(const uint256& txid, uint64_t fee_rate, uint64_t timestamp_ms) {
        received_txs_[txid] = TxInfo{fee_rate, timestamp_ms};

        CleanupOldEntries(timestamp_ms);
    }

    /**
     * Estimate mempool overlap with peer (0.0 - 1.0)
     * Based on transactions we've sent/received
     */
    double EstimateMempoolOverlap(const std::vector<uint256>& block_txids) const {
        if (block_txids.empty()) return 1.0;

        size_t likely_have = 0;
        for (const auto& txid : block_txids) {
            // Peer likely has tx if we sent it or received it from them
            if (sent_txs_.count(txid) > 0 || received_txs_.count(txid) > 0) {
                ++likely_have;
            }
        }

        return static_cast<double>(likely_have) / block_txids.size();
    }

    /**
     * Predict compact block reconstruction success probability
     * Combines mempool overlap with historical success rate
     */
    double PredictReconstructionSuccess(
        const std::vector<uint256>& block_txids,
        double historical_success_rate
    ) const {
        double overlap = EstimateMempoolOverlap(block_txids);

        // Weight: 70% mempool overlap, 30% historical success
        return 0.7 * overlap + 0.3 * historical_success_rate;
    }

    /**
     * Check if peer likely has transaction
     */
    bool PeerLikelyHasTx(const uint256& txid) const {
        return sent_txs_.count(txid) > 0 || received_txs_.count(txid) > 0;
    }

    /**
     * Get number of transactions we've tracked for this peer
     */
    size_t GetTrackedTxCount() const {
        return sent_txs_.size() + received_txs_.size();
    }

    /**
     * Get average fee rate of tracked transactions
     */
    uint64_t GetAverageFeeRate() const {
        if (sent_txs_.empty() && received_txs_.empty()) return 0;

        uint64_t total_fee_rate = 0;
        size_t count = 0;

        for (const auto& [txid, info] : sent_txs_) {
            total_fee_rate += info.fee_rate;
            ++count;
        }
        for (const auto& [txid, info] : received_txs_) {
            total_fee_rate += info.fee_rate;
            ++count;
        }

        return count > 0 ? total_fee_rate / count : 0;
    }

    /**
     * Clear all tracking data (for testing)
     */
    void Clear() {
        sent_txs_.clear();
        received_txs_.clear();
    }

private:
    struct TxInfo {
        uint64_t fee_rate;      // Una per byte
        uint64_t timestamp_ms;  // When we learned about this tx
    };

    /**
     * Remove transactions older than 10 minutes
     * (They're likely confirmed or replaced)
     */
    void CleanupOldEntries(uint64_t current_time_ms) {
        const uint64_t MAX_AGE_MS = 10 * 60 * 1000;  // 10 minutes

        // Cleanup sent_txs_
        for (auto it = sent_txs_.begin(); it != sent_txs_.end(); ) {
            if (current_time_ms - it->second.timestamp_ms > MAX_AGE_MS) {
                it = sent_txs_.erase(it);
            } else {
                ++it;
            }
        }

        // Cleanup received_txs_
        for (auto it = received_txs_.begin(); it != received_txs_.end(); ) {
            if (current_time_ms - it->second.timestamp_ms > MAX_AGE_MS) {
                it = received_txs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<uint256, TxInfo> sent_txs_;      // Transactions we sent to peer
    std::unordered_map<uint256, TxInfo> received_txs_;  // Transactions we received from peer
};

/**
 * Fee-aware transaction propagation
 * Prioritizes high-fee transactions for faster relay
 */
class FeeAwarePropagation {
public:
    /**
     * Determine if transaction should be relayed based on fee rate
     *
     * Strategy:
     * - Always relay high-fee txs (>= 10 una/byte)
     * - Relay medium-fee txs (>= 5 una/byte) with high probability
     * - Relay low-fee txs (>= 1 una/byte) with lower probability
     * - Drop dust txs (< 1 una/byte)
     */
    static bool ShouldRelay(uint64_t fee_rate_una_per_byte) {
        if (fee_rate_una_per_byte >= 10) {
            return true;  // High fee: always relay
        } else if (fee_rate_una_per_byte >= 5) {
            return true;  // Medium fee: always relay
        } else if (fee_rate_una_per_byte >= 1) {
            return true;  // Low fee: relay (could add probability here)
        } else {
            return false;  // Dust: drop
        }
    }

    /**
     * Get relay priority (higher = more urgent)
     * Used for queue ordering
     */
    static int GetRelayPriority(uint64_t fee_rate_una_per_byte) {
        if (fee_rate_una_per_byte >= 100) {
            return 4;  // Urgent: >100 una/byte
        } else if (fee_rate_una_per_byte >= 50) {
            return 3;  // High: 50-100 una/byte
        } else if (fee_rate_una_per_byte >= 10) {
            return 2;  // Medium: 10-50 una/byte
        } else if (fee_rate_una_per_byte >= 1) {
            return 1;  // Low: 1-10 una/byte
        } else {
            return 0;  // Dust: <1 una/byte
        }
    }

    /**
     * Estimate if transaction will be mined soon
     * Based on fee rate relative to network conditions
     */
    static bool LikelyToBeMinedSoon(uint64_t fee_rate_una_per_byte, uint64_t network_min_fee) {
        // Transaction likely to be mined if fee is at least 2x network minimum
        return fee_rate_una_per_byte >= (network_min_fee * 2);
    }
};

/**
 * Intelligent compact block peer selection
 * Combines mempool sync state with peer performance
 */
class IntelligentPeerSelector {
public:
    /**
     * Select best peer for sending compact block
     *
     * Considers:
     * - Mempool overlap (predicted reconstruction success)
     * - Peer performance score
     * - Recent compact block success rate
     */
    static std::string SelectBestPeer(
        const std::unordered_map<std::string, double>& peer_scores,
        const std::unordered_map<std::string, MempoolSyncTracker>& mempool_trackers,
        const std::vector<uint256>& block_txids,
        const std::unordered_map<std::string, double>& success_rates
    ) {
        std::string best_peer;
        double best_combined_score = -1.0;

        for (const auto& [peer_addr, peer_score] : peer_scores) {
            // Get mempool overlap
            double overlap = 0.5;  // Default if no tracker
            if (mempool_trackers.count(peer_addr) > 0) {
                overlap = mempool_trackers.at(peer_addr).EstimateMempoolOverlap(block_txids);
            }

            // Get historical success rate
            double success_rate = 0.5;  // Default
            if (success_rates.count(peer_addr) > 0) {
                success_rate = success_rates.at(peer_addr);
            }

            // Combined score: 40% peer score, 40% mempool overlap, 20% success rate
            double combined = 0.4 * (peer_score / 100.0) + 0.4 * overlap + 0.2 * success_rate;

            if (combined > best_combined_score) {
                best_combined_score = combined;
                best_peer = peer_addr;
            }
        }

        return best_peer;
    }
};

} // namespace dinero
