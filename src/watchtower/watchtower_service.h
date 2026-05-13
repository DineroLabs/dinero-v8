#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <memory>

// Forward declarations (L1 only - no Lightning headers)
struct Block;
struct Transaction;

// Forward declare IPC client (L1 → lightningd communication)
namespace dinero { namespace ipc { class LightningIPCClient; } }

namespace dinero {
namespace watchtower {

/**
 * @struct WatchedCommitment
 * @brief Minimal data needed to detect breach attempts
 *
 * Phase 9: Watchtower stores ONLY what's needed for txid matching.
 * No keys, no scripts, no balances, no delays.
 */
struct WatchedCommitment {
    std::string commitment_txid;  // Transaction ID to watch for (hex)
    std::string channel_id;       // Opaque channel identifier (hex)

    bool operator==(const WatchedCommitment& other) const {
        return commitment_txid == other.commitment_txid;
    }
};

/**
 * @class WatchtowerService
 * @brief L1-adjacent service that scans blocks and reports facts to lightningd
 *
 * Phase 9 Architecture:
 * - Lives in src/watchtower/ (NOT src/daemon/services/)
 * - Reads blocks from chainstate
 * - Matches txids against watched commitments
 * - Emits TransactionConfirmedEvent via IPC (facts only, no logic)
 * - Never accesses Lightning DB
 * - Never builds or signs transactions
 * - Never interprets breaches (Lightning decides meaning)
 *
 * Phase 8.5 Compliance:
 * - NO background threads (event-driven via onNewBlock callback)
 * - NO polling loops
 * - Purely reactive to blockchain events
 */
class WatchtowerService {
public:
    /**
     * @brief Construct watchtower service
     * @param db_path Path to watchtower database (for future persistence)
     * @param ipc_socket_path Path to lightningd IPC socket (optional)
     */
    explicit WatchtowerService(
        const std::string& db_path,
        const std::string& ipc_socket_path = "/tmp/lightningd.sock"
    );
    ~WatchtowerService();

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 9: Watched Commitment Management (Push Model)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Register revoked commitment for monitoring
     *
     * Called when Lightning learns about a revoked commitment (via IPC).
     * Push model: Lightning → IPC → Watchtower
     *
     * @param commitment Minimal commitment data
     */
    void addWatchedCommitment(const WatchedCommitment& commitment);

    /**
     * @brief Remove commitment from watch list
     *
     * Called when channel is closed or commitment is too old to matter.
     *
     * @param txid Transaction ID to stop watching
     */
    void removeWatchedCommitment(const std::string& txid);

    /**
     * @brief Get number of commitments being watched
     *
     * @return size_t Number of watched commitments
     */
    size_t getWatchedCommitmentCount() const;

    /**
     * @brief Clear all watched commitments (for testing)
     */
    void clearWatchedCommitments();

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 9: Block Scanning (Event-Driven)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Scan block for watched commitments
     *
     * Event-driven: Called by daemon on new block.
     * Algorithm:
     *   1. Iterate block.vtx
     *   2. For each tx, check if txid in watched set
     *   3. If match, emit TransactionConfirmedEvent via IPC
     *
     * Phase 9 rule: Emit facts only, no interpretation.
     *
     * @param block_hash Block hash (hex)
     * @param block_height Block height
     * @param transactions List of transactions in block
     * @return Number of watched commitments detected (0 if none)
     */
    uint32_t scanBlock(
        const std::string& block_hash,
        uint64_t block_height,
        const std::vector<Transaction>& transactions
    );

    // ═══════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════

    struct Stats {
        uint64_t blocks_scanned = 0;
        uint64_t transactions_scanned = 0;
        uint64_t breaches_detected = 0;  // "breach" = watched txid found (watchtower doesn't interpret)
        uint64_t false_positives = 0;    // Future: if Lightning reports "not actually a breach"
    };

    Stats getStats() const;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════

    std::string m_db_path;  // For future persistent storage

    // In-memory watched commitment set (Phase 9.1: minimal implementation)
    // Future: Persist to database for restart safety
    mutable std::mutex m_watched_mutex;
    std::unordered_set<std::string> m_watched_txids;                     // Fast O(1) lookup
    std::unordered_map<std::string, std::string> m_txid_to_channel_id;  // txid → channel_id

    // Statistics
    mutable std::mutex m_stats_mutex;
    Stats m_stats;

    // IPC Client (Phase 9: Send events to lightningd)
    std::unique_ptr<dinero::ipc::LightningIPCClient> m_ipc_client;

    // ═══════════════════════════════════════════════════════════════════════
    // IPC Communication (Phase 9)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Emit TransactionConfirmedEvent to lightningd via IPC
     *
     * Phase 9 rule: Send facts only, no logic.
     * Lightning decides if this is a breach, creates justice, etc.
     *
     * @param txid Transaction ID
     * @param channel_id Channel ID (opaque to watchtower)
     * @param block_height Block height where tx was confirmed
     */
    void emitTransactionConfirmed(
        const std::string& txid,
        const std::string& channel_id,
        uint64_t block_height
    );
};

} // namespace watchtower
} // namespace dinero
