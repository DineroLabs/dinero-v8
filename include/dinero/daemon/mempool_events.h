#pragma once

#include "wallet/transaction.h"
#include "compat/jsoncpp_compat.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <ctime>

namespace dinero {

/**
 * Mempool event types for WebSocket notifications
 */
enum class MempoolEventType {
    TRANSACTION_ACCEPTED,    // New transaction accepted to mempool
    TRANSACTION_REJECTED,    // Transaction rejected from mempool
    TRANSACTION_EVICTED,     // Transaction evicted due to limits
    TRANSACTION_CONFIRMED,   // Transaction confirmed in block
    MEMPOOL_SYNCED,         // Mempool sync completed
    FEE_ESTIMATE_UPDATED,   // Fee estimates updated
    ORPHAN_ADDED,           // Orphan transaction added
    ORPHAN_RESOLVED         // Orphan transaction resolved
};

/**
 * Mempool event data structure
 */
struct MempoolEvent {
    MempoolEventType type;
    std::string txid;
    uint64_t fee = 0;
    double feerate = 0.0;
    uint64_t size = 0;
    std::string reason;
    int64_t timestamp;
    
    // Additional context
    uint32_t mempool_size = 0;
    uint64_t mempool_bytes = 0;
    std::string peer_id;
    
    MempoolEvent(MempoolEventType t, const std::string& id = "")
        : type(t), txid(id), timestamp(std::time(nullptr)) {}
    
    // Convert to JSON for WebSocket
    Json::Value ToJson() const;
    
    // Create from transaction acceptance
    static MempoolEvent TransactionAccepted(const Transaction& tx, uint64_t fee, double feerate, uint64_t size);
    
    // Create from transaction rejection
    static MempoolEvent TransactionRejected(const std::string& txid, const std::string& reason);
    
    // Create from transaction eviction
    static MempoolEvent TransactionEvicted(const std::string& txid, const std::string& reason);
    
    // Create from transaction confirmation
    static MempoolEvent TransactionConfirmed(const std::string& txid, uint32_t block_height);
    
    // Create from mempool sync
    static MempoolEvent MempoolSynced(uint32_t tx_count, uint64_t total_bytes);
    
    // Create from fee estimate update
    static MempoolEvent FeeEstimateUpdated(double new_estimate);
    
    // Create from orphan events
    static MempoolEvent OrphanAdded(const std::string& txid, const std::string& peer_id);
    static MempoolEvent OrphanResolved(const std::string& txid, const std::string& parent_txid);
};

/**
 * Mempool event publisher for WebSocket notifications
 */
class MempoolEventPublisher {
public:
    using EventHandler = std::function<void(const MempoolEvent&)>;
    
    // Subscribe to mempool events
    void Subscribe(const EventHandler& handler);
    
    // Unsubscribe from events
    void Unsubscribe();
    
    // Publish event to all subscribers
    void Publish(const MempoolEvent& event);
    
    // Event creation helpers
    void PublishTransactionAccepted(const Transaction& tx, uint64_t fee);
    void PublishTransactionRejected(const std::string& txid, const std::string& reason);
    void PublishTransactionEvicted(const std::string& txid, const std::string& reason);
    void PublishTransactionConfirmed(const std::string& txid, uint32_t block_height);
    void PublishMempoolSynced(uint32_t tx_count, uint64_t total_bytes);
    void PublishFeeEstimateUpdated(double new_estimate);
    void PublishOrphanAdded(const std::string& txid, const std::string& peer_id);
    void PublishOrphanResolved(const std::string& txid, const std::string& parent_txid);
    
    // Statistics
    struct Stats {
        uint64_t events_published = 0;
        uint64_t subscribers_count = 0;
        uint64_t transactions_accepted = 0;
        uint64_t transactions_rejected = 0;
        uint64_t transactions_evicted = 0;
        uint64_t transactions_confirmed = 0;
        uint64_t orphans_added = 0;
        uint64_t orphans_resolved = 0;
    };
    
    Stats GetStats() const;
    
private:
    std::vector<EventHandler> subscribers_;
    mutable std::mutex mtx_;
    Stats stats_;
    
    void UpdateStats(MempoolEventType type);
};

/**
 * Subscription filter for mempool events
 */
struct MempoolSubscriptionFilter {
    std::vector<MempoolEventType> event_types;  // Empty = all events
    uint64_t min_fee = 0;                       // Minimum fee filter
    double min_feerate = 0.0;                   // Minimum fee rate filter
    std::vector<std::string> txid_filter;       // Specific transaction IDs
    bool include_orphans = true;                // Include orphan events
    bool include_confirmations = true;          // Include confirmation events
};

/**
 * WebSocket subscription manager for mempool events
 */
class MempoolWebSocketManager {
public:
    // WebSocket connection management
    void AddSubscription(const std::string& connection_id, const MempoolSubscriptionFilter& filter);
    void RemoveSubscription(const std::string& connection_id);
    void UpdateSubscription(const std::string& connection_id, const MempoolSubscriptionFilter& filter);
    
    // Event broadcasting
    void BroadcastEvent(const MempoolEvent& event);
    
    // Subscription queries
    std::vector<std::string> GetActiveSubscriptions() const;
    MempoolSubscriptionFilter GetSubscriptionFilter(const std::string& connection_id) const;
    
    // Statistics
    struct Stats {
        uint32_t active_subscriptions = 0;
        uint64_t events_broadcast = 0;
        uint64_t events_filtered = 0;
        uint64_t bytes_sent = 0;
    };
    
    Stats GetStats() const;
    
private:
    struct Subscription {
        std::string connection_id;
        MempoolSubscriptionFilter filter;
        int64_t created_time;
        uint64_t events_sent = 0;
    };
    
    std::unordered_map<std::string, Subscription> subscriptions_;
    mutable std::mutex mtx_;
    Stats stats_;
    
    bool ShouldSendEvent(const Subscription& sub, const MempoolEvent& event) const;
    void SendEventToConnection(const std::string& connection_id, const MempoolEvent& event);
};

/**
 * Global mempool event system
 */
extern std::unique_ptr<MempoolEventPublisher> g_mempool_events;
extern std::unique_ptr<MempoolWebSocketManager> g_mempool_websocket;

/**
 * Initialize mempool event system
 */
void InitializeMempoolEvents();

/**
 * Shutdown mempool event system
 */
void ShutdownMempoolEvents();

/**
 * Integration helpers for mempool
 */
namespace mempool_events {
    
    // Hook into TxMempool for automatic event publishing
    void OnTransactionAccepted(const Transaction& tx, uint64_t fee);
    void OnTransactionRejected(const std::string& txid, const std::string& reason);
    void OnTransactionEvicted(const std::string& txid, const std::string& reason);
    void OnTransactionConfirmed(const std::string& txid, uint32_t block_height);
    void OnMempoolSynced(uint32_t tx_count, uint64_t total_bytes);
    void OnFeeEstimateUpdated(double new_estimate);
    void OnOrphanAdded(const std::string& txid, const std::string& peer_id);
    void OnOrphanResolved(const std::string& txid, const std::string& parent_txid);
    
} // namespace mempool_events

} // namespace dinero
