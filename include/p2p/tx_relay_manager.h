#pragma once

#include "daemon/tx_mempool.h"
#include "wallet/transaction.h"
#include "consensus/chainparams.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <mutex>
#include <random>

namespace dinero {

// Forward declarations
class PeerManager;
class MetricsRegistry;

/**
 * P2P message types for transaction relay
 */
enum class P2PMessageType : uint32_t {
    INV = 1,
    GETDATA = 2,
    TX = 3,
    NOTFOUND = 4,
    FEEFILTER = 5,
    MEMPOOL = 6
};

/**
 * Inventory vector for announcing transactions
 */
struct InvVector {
    P2PMessageType type;
    std::string hash;  // Transaction ID or wtxid
    
    InvVector(P2PMessageType t, const std::string& h) : type(t), hash(h) {}
};

/**
 * Rolling filter for duplicate detection
 * Efficiently tracks recently seen items with bounded memory
 */
class RollingFilter {
public:
    explicit RollingFilter(size_t max_size = 10000);
    
    bool Contains(const std::string& item) const;
    void Insert(const std::string& item);
    void Clear();
    size_t Size() const { return items_.size(); }
    
private:
    mutable std::mutex mtx_;
    std::unordered_set<std::string> items_;
    std::vector<std::string> insertion_order_;
    size_t max_size_;
    size_t next_evict_idx_ = 0;
};

/**
 * Per-peer state for transaction relay
 */
struct PeerTxState {
    std::string peer_id;
    
    // Fee filtering (BIP133-like)
    uint64_t fee_filter = 0;  // Minimum fee rate (sat/kB)
    
    // Duplicate prevention
    RollingFilter recently_announced;    // TXIDs we've announced to this peer
    RollingFilter recently_requested;    // TXIDs we've requested from this peer
    
    // Send queue and rate limiting
    std::vector<InvVector> pending_invs;
    std::chrono::steady_clock::time_point last_inv_time;
    std::chrono::steady_clock::time_point last_tx_time;
    
    // Bandwidth tracking
    uint64_t bytes_sent_today = 0;
    uint64_t bytes_received_today = 0;
    std::chrono::steady_clock::time_point day_start;
    
    // Outstanding requests
    std::unordered_set<std::string> outstanding_getdata;
    size_t max_outstanding = 100;
    
    // DoS protection
    uint32_t misbehavior_score = 0;
    std::chrono::steady_clock::time_point last_misbehavior;
    
    explicit PeerTxState(const std::string& id) 
        : peer_id(id), recently_announced(5000), recently_requested(5000) {
        day_start = std::chrono::steady_clock::now();
    }
};

/**
 * Configuration for transaction relay manager
 */
struct TxRelayConfig {
    bool enabled = true;                    // Enable transaction relay
    uint32_t max_inv_batch_size = 35;      // Max invs per batch
    uint32_t max_peers_announce = 16;      // Max peers to announce to
    uint64_t max_bytes_per_peer_day = 100 * 1024 * 1024; // 100MB per day
    uint32_t trickle_delay_ms = 2000;      // Average trickle delay
    uint32_t max_orphan_requests = 10;     // Max orphan parent requests
    bool privacy_mode = false;             // Enhanced privacy features
};

/**
 * Transaction relay manager for P2P mempool synchronization
 * 
 * Handles:
 * - Transaction announcements (inv messages)
 * - Transaction requests (getdata messages) 
 * - Transaction broadcasting
 * - Fee filtering and duplicate prevention
 * - Rate limiting and DoS protection
 */
class TxRelayManager {
public:
    
    explicit TxRelayManager(
        TxMempool& mempool,
        const TxRelayConfig& config = TxRelayConfig()
    );
    
    ~TxRelayManager();
    
    // Lifecycle
    void Start();
    void Stop();
    bool IsRunning() const { return running_; }
    
    // Peer management
    void AddPeer(const std::string& peer_id);
    void RemovePeer(const std::string& peer_id);
    void SetPeerFeeFilter(const std::string& peer_id, uint64_t min_feerate);
    
    // Event handlers (called by mempool/network layer)
    void OnTransactionAccepted(const Transaction& tx, uint64_t fee);
    void OnBlockConnected(const std::vector<std::string>& tx_hashes);
    void OnPeerInv(const std::string& peer_id, const std::vector<InvVector>& invs);
    void OnPeerGetData(const std::string& peer_id, const std::vector<std::string>& tx_hashes);
    void OnPeerTx(const std::string& peer_id, const Transaction& tx);
    void OnPeerFeeFilter(const std::string& peer_id, uint64_t min_feerate);
    void OnPeerMempoolRequest(const std::string& peer_id);
    
    // Periodic processing
    void ProcessTrickleQueue();
    void ProcessTimeouts();
    void UpdateMetrics();
    
    // Statistics and monitoring
    struct Stats {
        uint64_t transactions_announced = 0;
        uint64_t transactions_requested = 0;
        uint64_t transactions_relayed = 0;
        uint64_t transactions_dropped = 0;
        uint64_t inv_messages_sent = 0;
        uint64_t getdata_messages_sent = 0;
        uint64_t bytes_relayed = 0;
        uint32_t active_peers = 0;
        uint32_t pending_requests = 0;
    };
    
    Stats GetStats() const;
    std::vector<std::string> GetConnectedPeers() const;
    
    // Configuration
    void UpdateConfig(const TxRelayConfig& config);
    const TxRelayConfig& GetConfig() const { return config_; }
    
private:
    // Core components
    TxMempool& mempool_;
    TxRelayConfig config_;
    
    // State management
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<PeerTxState>> peers_;
    bool running_ = false;
    
    // Trickle queue for privacy
    struct TrickleEntry {
        std::string txid;
        std::chrono::steady_clock::time_point announce_time;
        std::vector<std::string> target_peers;
    };
    std::vector<TrickleEntry> trickle_queue_;
    
    // Random number generation
    mutable std::mt19937 rng_;
    
    // Statistics
    mutable Stats stats_;
    
    // Internal methods
    void AnnounceToAllPeers(const std::string& txid, uint64_t fee);
    void AnnounceToSpecificPeers(const std::string& txid, const std::vector<std::string>& peer_ids);
    void RequestTransaction(const std::string& peer_id, const std::string& txid);
    void SendTransaction(const std::string& peer_id, const Transaction& tx);
    void SendInvBatch(const std::string& peer_id, const std::vector<InvVector>& invs);
    
    // Filtering and validation
    bool ShouldAnnounceToPeer(const std::string& peer_id, const std::string& txid, uint64_t fee) const;
    bool ShouldRequestFromPeer(const std::string& peer_id, const std::string& txid) const;
    std::vector<std::string> SelectAnnouncePeers(const std::string& txid, uint64_t fee) const;
    
    // Rate limiting and DoS protection
    bool CheckRateLimit(const std::string& peer_id, size_t bytes) const;
    void UpdateBandwidthUsage(const std::string& peer_id, size_t bytes, bool outbound);
    void IncrementMisbehavior(const std::string& peer_id, uint32_t score);
    
    // Privacy features
    std::chrono::milliseconds CalculateTrickleDelay() const;
    void ShufflePeers(std::vector<std::string>& peers) const;
    
    // Cleanup and maintenance
    void CleanupExpiredRequests();
    void CleanupOldBandwidthData();
    
    // Metrics helpers
    void IncrementCounter(const std::string& metric_name, uint64_t value = 1);
    void SetGauge(const std::string& metric_name, double value);
};

/**
 * P2P message structures for transaction relay
 */
namespace p2p_messages {
    
    struct InvMessage {
        std::vector<InvVector> inventory;
        
        std::string Serialize() const;
        bool Deserialize(const std::string& data);
    };
    
    struct GetDataMessage {
        std::vector<InvVector> inventory;
        
        std::string Serialize() const;
        bool Deserialize(const std::string& data);
    };
    
    struct TxMessage {
        Transaction transaction;
        
        std::string Serialize() const;
        bool Deserialize(const std::string& data);
    };
    
    struct FeeFilterMessage {
        uint64_t feerate; // sat/kB
        
        std::string Serialize() const;
        bool Deserialize(const std::string& data);
    };
    
    struct NotFoundMessage {
        std::vector<InvVector> inventory;
        
        std::string Serialize() const;
        bool Deserialize(const std::string& data);
    };
    
} // namespace p2p_messages

/**
 * Global transaction relay manager instance
 */
extern std::unique_ptr<TxRelayManager> g_tx_relay_manager;

/**
 * Initialize transaction relay manager
 */
void InitializeTxRelay(TxMempool& mempool, const TxRelayConfig& config = TxRelayConfig());

/**
 * Shutdown transaction relay manager
 */
void ShutdownTxRelay();

} // namespace dinero
