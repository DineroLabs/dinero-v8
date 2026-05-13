#include "watchtower_service.h"
#include "ipc/lightning_ipc_client.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {
namespace watchtower {

WatchtowerService::WatchtowerService(
    const std::string& db_path,
    const std::string& ipc_socket_path
)
    : m_db_path(db_path)
    , m_ipc_client(std::make_unique<dinero::ipc::LightningIPCClient>(ipc_socket_path))
{
    g_logger.info("⚡ WatchtowerService: Initialized (Phase 9: L1-adjacent block scanner)");
    g_logger.info("⚡ Database path: " + m_db_path);
    g_logger.info("⚡ IPC socket: " + ipc_socket_path);
    g_logger.info("⚡ Storage: In-memory (Phase 9.1 - persistence TODO)");
}

WatchtowerService::~WatchtowerService() {
    g_logger.info("⚡ WatchtowerService: Shutdown (watched commitments: " +
                 std::to_string(getWatchedCommitmentCount()) + ")");
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9: Watched Commitment Management (Push Model)
// ═══════════════════════════════════════════════════════════════════════════

void WatchtowerService::addWatchedCommitment(const WatchedCommitment& commitment) {
    std::lock_guard<std::mutex> lock(m_watched_mutex);

    // Add to fast lookup set
    m_watched_txids.insert(commitment.commitment_txid);

    // Store channel mapping (for IPC event)
    m_txid_to_channel_id[commitment.commitment_txid] = commitment.channel_id;

    g_logger.info("⚡ WatchtowerService: Watching commitment txid=" +
                 commitment.commitment_txid.substr(0, 16) + "... " +
                 "channel=" + commitment.channel_id.substr(0, 16) + "...");
    g_logger.info("⚡ Total watched commitments: " + std::to_string(m_watched_txids.size()));
}

void WatchtowerService::removeWatchedCommitment(const std::string& txid) {
    std::lock_guard<std::mutex> lock(m_watched_mutex);

    m_watched_txids.erase(txid);
    m_txid_to_channel_id.erase(txid);

    g_logger.debug("⚡ WatchtowerService: Removed commitment txid=" + txid.substr(0, 16) + "...");
}

size_t WatchtowerService::getWatchedCommitmentCount() const {
    std::lock_guard<std::mutex> lock(m_watched_mutex);
    return m_watched_txids.size();
}

void WatchtowerService::clearWatchedCommitments() {
    std::lock_guard<std::mutex> lock(m_watched_mutex);
    m_watched_txids.clear();
    m_txid_to_channel_id.clear();
    g_logger.info("⚡ WatchtowerService: Cleared all watched commitments");
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 9: Block Scanning (Event-Driven)
// ═══════════════════════════════════════════════════════════════════════════

uint32_t WatchtowerService::scanBlock(
    const std::string& block_hash,
    uint64_t block_height,
    const std::vector<Transaction>& transactions
) {
    g_logger.debug("⚡ WatchtowerService: Scanning block height=" +
                  std::to_string(block_height) +
                  " hash=" + block_hash.substr(0, 16) + "... " +
                  "txs=" + std::to_string(transactions.size()));

    uint32_t breaches_detected = 0;

    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
        m_stats.blocks_scanned++;
        m_stats.transactions_scanned += transactions.size();
    }

    // Phase 9: Scan all transactions in block
    for (const auto& tx : transactions) {
        // Get transaction ID
        std::string txid = tx.getTxid();  // Assumes Transaction has getTxid() method

        // Fast O(1) lookup: Is this txid being watched?
        std::string channel_id;
        bool is_watched = false;

        {
            std::lock_guard<std::mutex> lock(m_watched_mutex);
            auto it = m_watched_txids.find(txid);
            if (it != m_watched_txids.end()) {
                is_watched = true;
                channel_id = m_txid_to_channel_id[txid];
            }
        }

        if (is_watched) {
            // WATCHED COMMITMENT DETECTED ON-CHAIN!
            g_logger.warning("⚡ ⚡ ⚡ WATCHTOWER DETECTED WATCHED COMMITMENT ⚡ ⚡ ⚡");
            g_logger.warning("⚡ Block height: " + std::to_string(block_height));
            g_logger.warning("⚡ Transaction: " + txid);
            g_logger.warning("⚡ Channel: " + channel_id.substr(0, 16) + "...");
            g_logger.warning("⚡ Emitting fact to lightningd (Lightning decides if breach)");

            // Phase 9: Emit fact (not interpretation) to Lightning via IPC
            emitTransactionConfirmed(txid, channel_id, block_height);

            breaches_detected++;

            // Update statistics
            {
                std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
                m_stats.breaches_detected++;
            }
        }
    }

    if (breaches_detected > 0) {
        g_logger.warning("⚡ WatchtowerService: Block scan complete - " +
                        std::to_string(breaches_detected) + " watched commitment(s) detected");
    } else {
        g_logger.debug("⚡ WatchtowerService: Block scan complete - no watched commitments detected");
    }

    return breaches_detected;
}

// ═══════════════════════════════════════════════════════════════════════════
// Statistics
// ═══════════════════════════════════════════════════════════════════════════

WatchtowerService::Stats WatchtowerService::getStats() const {
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    return m_stats;
}

// ═══════════════════════════════════════════════════════════════════════════
// IPC Communication (Phase 9)
// ═══════════════════════════════════════════════════════════════════════════

void WatchtowerService::emitTransactionConfirmed(
    const std::string& txid,
    const std::string& channel_id,
    uint64_t block_height
) {
    // Phase 9: Emit TransactionConfirmedEvent to lightningd via IPC
    //
    // Rule: Send facts only, no interpretation.
    // Lightning decides:
    //   - Is this actually a revoked commitment?
    //   - Is CSV satisfied?
    //   - Should we create justice transaction?
    //   - Is it already handled (idempotency)?

    g_logger.info("⚡ WatchtowerService: IPC → lightningd");
    g_logger.info("⚡   Event: TransactionConfirmedEvent");
    g_logger.info("⚡   txid: " + txid);
    g_logger.info("⚡   channel_id: " + channel_id + " (opaque to watchtower)");
    g_logger.info("⚡   block_height: " + std::to_string(block_height));

    // Send TX_CONFIRMED event via IPC
    // Note: channel_id is NOT sent - Lightning already knows which txids matter
    bool sent = m_ipc_client->sendTxConfirmed(txid, block_height);

    if (sent) {
        g_logger.info("⚡ IPC message sent successfully");
    } else {
        g_logger.warning("⚡ IPC message failed (lightningd not running or connection issue)");
        g_logger.warning("⚡ This is OK - Lightning will catch up on restart via chain replay");
    }
}

} // namespace watchtower
} // namespace dinero
