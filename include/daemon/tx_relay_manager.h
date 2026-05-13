#pragma once

/**
 * Phase G.3: Mempool Relay (Skeleton)
 *
 * Scope:
 * - Announce transactions via inv messages
 * - Handle getdata requests for transactions
 * - Handle incoming tx messages
 * - Route transactions through MempoolService validation
 *
 * NOT in scope:
 * - Transaction prioritization
 * - Fee-based relay policies
 * - Bloom filters
 * - Compact transaction relay
 *
 * Safety:
 * - Every transaction goes through MempoolService::addTransaction()
 * - Never mutate mempool directly
 * - Never bypass mempool validation
 * - Duplicate/replay protection via seen_txs set
 */

#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "daemon/interfaces/ingress_types.h"  // TxAcceptResult, TxRejectCode
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <memory>

namespace dinero {

// Forward declarations
class P2PManager;
class MempoolService;
class ILogger;
class TxOrphanPool;

/**
 * TxRelayManager - Phase G.3 minimal transaction propagation
 *
 * Responsibilities:
 * 1. Announce transactions after successful mempool validation
 * 2. Handle getdata requests for known transactions
 * 3. Route incoming transactions to mempool validation
 * 4. Prevent duplicate transaction processing
 */
class TxRelayManager {
public:
    /**
     * Callback type for sending P2P messages
     * @param peer_address Peer to send to ("" = broadcast)
     * @param command Message command (inv, getdata, tx)
     * @param payload Message payload
     */
    using SendMessageCallback = std::function<void(
        const std::string& peer_address,
        const std::string& command,
        const std::vector<uint8_t>& payload
    )>;

    /**
     * Callback type for transaction validation request
     * @param tx Transaction to validate
     * @param peer_address Peer that sent the transaction
     * @return true if transaction was accepted
     */
    using ValidateTxCallback = std::function<bool(
        const Transaction& tx,
        const std::string& peer_address
    )>;

    /**
     * Callback type for transaction retrieval from mempool
     * @param txid Transaction ID to retrieve
     * @param out_tx Output parameter for retrieved transaction
     * @return true if transaction was found and retrieved
     */
    using RetrieveTxCallback = std::function<bool(
        const uint256& txid,
        Transaction& out_tx
    )>;

    /**
     * Constructor
     * @param logger Logger instance
     */
    explicit TxRelayManager(ILogger* logger);

    ~TxRelayManager() = default;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set callback for sending P2P messages
     */
    void SetSendMessageCallback(SendMessageCallback callback) {
        send_message_callback_ = callback;
    }

    /**
     * Set callback for transaction validation
     */
    void SetValidateTxCallback(ValidateTxCallback callback) {
        validate_tx_callback_ = callback;
    }

    /**
     * Set callback for transaction retrieval from mempool
     */
    void SetRetrieveTxCallback(RetrieveTxCallback callback) {
        retrieve_tx_callback_ = callback;
    }

    /**
     * Callback type for structured transaction submission (returns reject code).
     * When set, this is preferred over ValidateTxCallback because it enables
     * orphan pool integration (MISSING_INPUTS detection).
     */
    using SubmitTxCallback = std::function<TxAcceptResult(
        const Transaction& tx,
        const std::string& peer_address
    )>;

    /**
     * Set callback for structured transaction submission.
     * Preferred over SetValidateTxCallback when orphan pool is enabled.
     */
    void SetSubmitTxCallback(SubmitTxCallback callback) {
        submit_tx_callback_ = callback;
    }

    /**
     * Set the transaction orphan pool (non-owning pointer).
     * Enables holding transactions with missing inputs for later resolution.
     */
    void SetOrphanPool(TxOrphanPool* pool) { orphan_pool_ = pool; }

    /**
     * Get the transaction orphan pool (for RPC diagnostics).
     */
    TxOrphanPool* GetOrphanPool() const { return orphan_pool_; }

    // ========================================================================
    // Transaction Announcement (Outbound)
    // ========================================================================

    /**
     * Announce a transaction to all peers
     *
     * Called by MempoolService after transaction validation succeeds.
     * Broadcasts inv(MSG_TX, txid) to all connected peers.
     *
     * Safety: Only announce transactions that passed mempool validation.
     *
     * @param txid Transaction ID of successfully validated transaction
     */
    void AnnounceTx(const uint256& txid);

    // ========================================================================
    // Transaction Request Handling (Inbound)
    // ========================================================================

    /**
     * Handle inv message from peer
     *
     * If transaction is unknown, request it via getdata.
     * If transaction is known (in seen_txs), ignore.
     *
     * @param peer_address Peer that sent inv
     * @param txid Transaction ID from inv message
     */
    void HandleInv(const std::string& peer_address, const uint256& txid);

    /**
     * Handle getdata request from peer
     *
     * If we have the transaction, send it to the requesting peer.
     * Uses mempool to retrieve transaction.
     *
     * @param peer_address Peer that requested transaction
     * @param txid Requested transaction ID
     */
    void HandleGetData(const std::string& peer_address, const uint256& txid);

    /**
     * Handle incoming tx message
     *
     * Routes transaction to mempool validation via callback.
     * If validation succeeds, adds to seen_txs.
     * If validation fails, transaction is rejected (no relay).
     *
     * @param peer_address Peer that sent transaction
     * @param tx Transaction data
     */
    void HandleTx(const std::string& peer_address, const Transaction& tx);

    // ========================================================================
    // Proof Refresh (#6)
    // ========================================================================

    /**
     * Request fresh proofs for stale mempool TXs from bridge peers.
     * Sends getdata(MSG_UTREEXO_TX) for up to batch_size txids.
     * Rate-limited to avoid flooding bridge peers.
     */
    void RequestProofRefresh(const std::vector<uint256>& stale_txids, size_t batch_size = 20);

    /**
     * Notify relay manager that chain tip changed.
     *
     * Invalidates in-flight refresh tracking so stale requests from the
     * previous tip do not block refresh at the new tip.
     */
    void OnTipChanged();

    /**
     * Record that a peer successfully served a utxotx message.
     * Makes this peer preferred for future refresh requests.
     */
    void RecordBridgeResponse(const std::string& peer_addr);

    /**
     * Remove a txid from the pending-refresh set (proof arrived or TX evicted).
     */
    void CompleteRefresh(const uint256& txid);

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * Check if we've already seen this transaction
     */
    bool IsTxSeen(const uint256& txid) const;

    /**
     * Get count of seen transactions (for debugging)
     */
    size_t GetSeenTxCount() const;

    /**
     * Enable CSN mode: getdata uses MSG_UTREEXO_TX instead of MSG_TX
     */
    void SetCsnMode(bool csn) { csn_mode_ = csn; }

private:
    // Logger
    ILogger* logger_;

    // Callbacks
    SendMessageCallback send_message_callback_;
    ValidateTxCallback validate_tx_callback_;
    SubmitTxCallback submit_tx_callback_;
    RetrieveTxCallback retrieve_tx_callback_;

    // Transaction orphan pool (non-owning — lifetime managed by DaemonApp)
    TxOrphanPool* orphan_pool_ = nullptr;

    // Lazy orphan expiry tracking
    std::chrono::steady_clock::time_point last_orphan_expiry_;

    // Phase #4: CSN mode — use MSG_UTREEXO_TX in getdata
    bool csn_mode_ = false;

    // Seen transactions (duplicate prevention)
    mutable std::mutex seen_txs_mutex_;
    std::unordered_set<uint256> seen_txs_;

    // Proof refresh state (#6)
    mutable std::mutex refresh_mutex_;
    std::unordered_map<uint256, std::chrono::steady_clock::time_point> pending_refresh_;
    std::unordered_set<std::string> bridge_capable_peers_;
    size_t refresh_rr_index_ = 0;
    std::chrono::steady_clock::time_point last_refresh_batch_;
    static constexpr size_t MAX_PENDING_REFRESH = 100;
    static constexpr std::chrono::milliseconds REFRESH_COOLDOWN{500};
    static constexpr std::chrono::milliseconds REFRESH_REQUEST_TIMEOUT{5000};

    // Helper: Add transaction to seen set
    void MarkTxAsSeen(const uint256& txid);

    // Helper: Serialize inv message
    std::vector<uint8_t> SerializeInv(const uint256& txid) const;

    // Helper: Serialize getdata message
    std::vector<uint8_t> SerializeGetData(const uint256& txid) const;

    // Helper: Serialize tx message
    std::vector<uint8_t> SerializeTx(const Transaction& tx) const;
};

} // namespace dinero
