#include "daemon/mempool_events.h"
#include "daemon/tx_mempool.h"
#include "common/logger.h"
#include <chrono>
#include <algorithm>

namespace dinero {

// Global instances
std::unique_ptr<MempoolEventPublisher> g_mempool_events;
std::unique_ptr<MempoolWebSocketManager> g_mempool_websocket;

// MempoolEvent implementation
Json::Value MempoolEvent::ToJson() const {
    Json::Value json;
    
    // Event type
    std::string type_str;
    switch (type) {
        case MempoolEventType::TRANSACTION_ACCEPTED: type_str = "transaction_accepted"; break;
        case MempoolEventType::TRANSACTION_REJECTED: type_str = "transaction_rejected"; break;
        case MempoolEventType::TRANSACTION_EVICTED: type_str = "transaction_evicted"; break;
        case MempoolEventType::TRANSACTION_CONFIRMED: type_str = "transaction_confirmed"; break;
        case MempoolEventType::MEMPOOL_SYNCED: type_str = "mempool_synced"; break;
        case MempoolEventType::FEE_ESTIMATE_UPDATED: type_str = "fee_estimate_updated"; break;
        case MempoolEventType::ORPHAN_ADDED: type_str = "orphan_added"; break;
        case MempoolEventType::ORPHAN_RESOLVED: type_str = "orphan_resolved"; break;
    }
    
    json["type"] = type_str;
    json["timestamp"] = static_cast<Json::Int64>(timestamp);
    
    // Transaction data
    if (!txid.empty()) {
        json["txid"] = txid;
    }
    
    if (fee > 0) {
        json["fee"] = static_cast<Json::Int64>(fee);
    }
    
    if (feerate > 0.0) {
        json["feerate"] = feerate;
    }
    
    if (size > 0) {
        json["size"] = static_cast<Json::Int64>(size);
    }
    
    if (!reason.empty()) {
        json["reason"] = reason;
    }
    
    if (!peer_id.empty()) {
        json["peer_id"] = peer_id;
    }
    
    // Mempool context
    if (mempool_size > 0) {
        json["mempool_size"] = mempool_size;
    }
    
    if (mempool_bytes > 0) {
        json["mempool_bytes"] = static_cast<Json::Int64>(mempool_bytes);
    }
    
    return json;
}

MempoolEvent MempoolEvent::TransactionAccepted(const Transaction& tx, uint64_t fee, double feerate, uint64_t size) {
    MempoolEvent event(MempoolEventType::TRANSACTION_ACCEPTED, tx.GetTxId());
    event.fee = fee;
    event.feerate = feerate;
    event.size = size;
    return event;
}

MempoolEvent MempoolEvent::TransactionRejected(const std::string& txid, const std::string& reason) {
    MempoolEvent event(MempoolEventType::TRANSACTION_REJECTED, txid);
    event.reason = reason;
    return event;
}

MempoolEvent MempoolEvent::TransactionEvicted(const std::string& txid, const std::string& reason) {
    MempoolEvent event(MempoolEventType::TRANSACTION_EVICTED, txid);
    event.reason = reason;
    return event;
}

MempoolEvent MempoolEvent::TransactionConfirmed(const std::string& txid, uint32_t block_height) {
    MempoolEvent event(MempoolEventType::TRANSACTION_CONFIRMED, txid);
    event.reason = "block_" + std::to_string(block_height);
    return event;
}

MempoolEvent MempoolEvent::MempoolSynced(uint32_t tx_count, uint64_t total_bytes) {
    MempoolEvent event(MempoolEventType::MEMPOOL_SYNCED);
    event.mempool_size = tx_count;
    event.mempool_bytes = total_bytes;
    return event;
}

MempoolEvent MempoolEvent::FeeEstimateUpdated(double new_estimate) {
    MempoolEvent event(MempoolEventType::FEE_ESTIMATE_UPDATED);
    event.feerate = new_estimate;
    return event;
}

MempoolEvent MempoolEvent::OrphanAdded(const std::string& txid, const std::string& peer_id) {
    MempoolEvent event(MempoolEventType::ORPHAN_ADDED, txid);
    event.peer_id = peer_id;
    return event;
}

MempoolEvent MempoolEvent::OrphanResolved(const std::string& txid, const std::string& parent_txid) {
    MempoolEvent event(MempoolEventType::ORPHAN_RESOLVED, txid);
    event.reason = "parent_" + parent_txid;
    return event;
}

// MempoolEventPublisher implementation
void MempoolEventPublisher::Subscribe(const EventHandler& handler) {
    std::lock_guard<std::mutex> lock(mtx_);
    subscribers_.push_back(handler);
    stats_.subscribers_count = subscribers_.size();
}

void MempoolEventPublisher::Unsubscribe() {
    std::lock_guard<std::mutex> lock(mtx_);
    subscribers_.clear();
    stats_.subscribers_count = 0;
}

void MempoolEventPublisher::Publish(const MempoolEvent& event) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    for (const auto& handler : subscribers_) {
        try {
            handler(event);
        } catch (const std::exception& e) {
            dinero::g_logger.warning("Mempool event handler error: " + std::string(e.what()));
        }
    }
    
    UpdateStats(event.type);
    stats_.events_published++;
}

void MempoolEventPublisher::PublishTransactionAccepted(const Transaction& tx, uint64_t fee) {
    TxMempoolEntry temp_entry(tx, fee, 0);
    double feerate = static_cast<double>(fee) / temp_entry.GetVSize();
    auto event = MempoolEvent::TransactionAccepted(tx, fee, feerate, temp_entry.GetSize());
    Publish(event);
}

void MempoolEventPublisher::PublishTransactionRejected(const std::string& txid, const std::string& reason) {
    auto event = MempoolEvent::TransactionRejected(txid, reason);
    Publish(event);
}

void MempoolEventPublisher::PublishTransactionEvicted(const std::string& txid, const std::string& reason) {
    auto event = MempoolEvent::TransactionEvicted(txid, reason);
    Publish(event);
}

void MempoolEventPublisher::PublishTransactionConfirmed(const std::string& txid, uint32_t block_height) {
    auto event = MempoolEvent::TransactionConfirmed(txid, block_height);
    Publish(event);
}

void MempoolEventPublisher::PublishMempoolSynced(uint32_t tx_count, uint64_t total_bytes) {
    auto event = MempoolEvent::MempoolSynced(tx_count, total_bytes);
    Publish(event);
}

void MempoolEventPublisher::PublishFeeEstimateUpdated(double new_estimate) {
    auto event = MempoolEvent::FeeEstimateUpdated(new_estimate);
    Publish(event);
}

void MempoolEventPublisher::PublishOrphanAdded(const std::string& txid, const std::string& peer_id) {
    auto event = MempoolEvent::OrphanAdded(txid, peer_id);
    Publish(event);
}

void MempoolEventPublisher::PublishOrphanResolved(const std::string& txid, const std::string& parent_txid) {
    auto event = MempoolEvent::OrphanResolved(txid, parent_txid);
    Publish(event);
}

MempoolEventPublisher::Stats MempoolEventPublisher::GetStats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

void MempoolEventPublisher::UpdateStats(MempoolEventType type) {
    switch (type) {
        case MempoolEventType::TRANSACTION_ACCEPTED: stats_.transactions_accepted++; break;
        case MempoolEventType::TRANSACTION_REJECTED: stats_.transactions_rejected++; break;
        case MempoolEventType::TRANSACTION_EVICTED: stats_.transactions_evicted++; break;
        case MempoolEventType::TRANSACTION_CONFIRMED: stats_.transactions_confirmed++; break;
        case MempoolEventType::ORPHAN_ADDED: stats_.orphans_added++; break;
        case MempoolEventType::ORPHAN_RESOLVED: stats_.orphans_resolved++; break;
        default: break;
    }
}

// MempoolWebSocketManager implementation
void MempoolWebSocketManager::AddSubscription(const std::string& connection_id, const MempoolSubscriptionFilter& filter) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    Subscription sub;
    sub.connection_id = connection_id;
    sub.filter = filter;
    sub.created_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    subscriptions_[connection_id] = sub;
    stats_.active_subscriptions = subscriptions_.size();
    
    dinero::g_logger.debug("Added mempool WebSocket subscription: " + connection_id);
}

void MempoolWebSocketManager::RemoveSubscription(const std::string& connection_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = subscriptions_.find(connection_id);
    if (it != subscriptions_.end()) {
        subscriptions_.erase(it);
        stats_.active_subscriptions = subscriptions_.size();
        dinero::g_logger.debug("Removed mempool WebSocket subscription: " + connection_id);
    }
}

void MempoolWebSocketManager::UpdateSubscription(const std::string& connection_id, const MempoolSubscriptionFilter& filter) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = subscriptions_.find(connection_id);
    if (it != subscriptions_.end()) {
        it->second.filter = filter;
        dinero::g_logger.debug("Updated mempool WebSocket subscription: " + connection_id);
    }
}

void MempoolWebSocketManager::BroadcastEvent(const MempoolEvent& event) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    for (auto& [connection_id, subscription] : subscriptions_) {
        if (ShouldSendEvent(subscription, event)) {
            SendEventToConnection(connection_id, event);
            subscription.events_sent++;
            stats_.events_broadcast++;
        } else {
            stats_.events_filtered++;
        }
    }
}

std::vector<std::string> MempoolWebSocketManager::GetActiveSubscriptions() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<std::string> connections;
    connections.reserve(subscriptions_.size());
    
    for (const auto& [connection_id, subscription] : subscriptions_) {
        connections.push_back(connection_id);
    }
    
    return connections;
}

MempoolSubscriptionFilter MempoolWebSocketManager::GetSubscriptionFilter(const std::string& connection_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto it = subscriptions_.find(connection_id);
    if (it != subscriptions_.end()) {
        return it->second.filter;
    }
    
    return MempoolSubscriptionFilter{};
}

MempoolWebSocketManager::Stats MempoolWebSocketManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

bool MempoolWebSocketManager::ShouldSendEvent(const Subscription& sub, const MempoolEvent& event) const {
    const auto& filter = sub.filter;
    
    // Check event type filter
    if (!filter.event_types.empty()) {
        if (std::find(filter.event_types.begin(), filter.event_types.end(), event.type) == filter.event_types.end()) {
            return false;
        }
    }
    
    // Check fee filters
    if (event.fee > 0 && event.fee < filter.min_fee) {
        return false;
    }
    
    if (event.feerate > 0.0 && event.feerate < filter.min_feerate) {
        return false;
    }
    
    // Check transaction ID filter
    if (!filter.txid_filter.empty() && !event.txid.empty()) {
        if (std::find(filter.txid_filter.begin(), filter.txid_filter.end(), event.txid) == filter.txid_filter.end()) {
            return false;
        }
    }
    
    // Check orphan filter
    if (!filter.include_orphans && 
        (event.type == MempoolEventType::ORPHAN_ADDED || event.type == MempoolEventType::ORPHAN_RESOLVED)) {
        return false;
    }
    
    // Check confirmation filter
    if (!filter.include_confirmations && event.type == MempoolEventType::TRANSACTION_CONFIRMED) {
        return false;
    }
    
    return true;
}

void MempoolWebSocketManager::SendEventToConnection(const std::string& connection_id, const MempoolEvent& event) {
    // This would integrate with the actual WebSocket server
    // For now, just log the event
    std::string event_json = event.ToJson().toStyledString();
    stats_.bytes_sent += event_json.size();
    
    dinero::g_logger.debug("Sending mempool event to " + connection_id + ": " + event.ToJson()["type"].asString());
}

// Global functions
void InitializeMempoolEvents() {
    if (g_mempool_events || g_mempool_websocket) {
        dinero::g_logger.warning("Mempool events already initialized");
        return;
    }
    
    g_mempool_events = std::make_unique<MempoolEventPublisher>();
    g_mempool_websocket = std::make_unique<MempoolWebSocketManager>();
    
    // Subscribe WebSocket manager to events
    g_mempool_events->Subscribe([](const MempoolEvent& event) {
        if (g_mempool_websocket) {
            g_mempool_websocket->BroadcastEvent(event);
        }
    });
    
    dinero::g_logger.info("✅ Mempool event system initialized");
}

void ShutdownMempoolEvents() {
    if (g_mempool_events) {
        g_mempool_events->Unsubscribe();
        g_mempool_events.reset();
    }
    
    if (g_mempool_websocket) {
        g_mempool_websocket.reset();
    }
    
    dinero::g_logger.info("Mempool event system shutdown");
}

// Integration helpers
namespace mempool_events {
    
    void OnTransactionAccepted(const Transaction& tx, uint64_t fee) {
        if (g_mempool_events) {
            g_mempool_events->PublishTransactionAccepted(tx, fee);
        }
    }
    
    void OnTransactionRejected(const std::string& txid, const std::string& reason) {
        if (g_mempool_events) {
            g_mempool_events->PublishTransactionRejected(txid, reason);
        }
    }
    
    void OnTransactionEvicted(const std::string& txid, const std::string& reason) {
        if (g_mempool_events) {
            g_mempool_events->PublishTransactionEvicted(txid, reason);
        }
    }
    
    void OnTransactionConfirmed(const std::string& txid, uint32_t block_height) {
        if (g_mempool_events) {
            g_mempool_events->PublishTransactionConfirmed(txid, block_height);
        }
    }
    
    void OnMempoolSynced(uint32_t tx_count, uint64_t total_bytes) {
        if (g_mempool_events) {
            g_mempool_events->PublishMempoolSynced(tx_count, total_bytes);
        }
    }
    
    void OnFeeEstimateUpdated(double new_estimate) {
        if (g_mempool_events) {
            g_mempool_events->PublishFeeEstimateUpdated(new_estimate);
        }
    }
    
    void OnOrphanAdded(const std::string& txid, const std::string& peer_id) {
        if (g_mempool_events) {
            g_mempool_events->PublishOrphanAdded(txid, peer_id);
        }
    }
    
    void OnOrphanResolved(const std::string& txid, const std::string& parent_txid) {
        if (g_mempool_events) {
            g_mempool_events->PublishOrphanResolved(txid, parent_txid);
        }
    }
    
} // namespace mempool_events

} // namespace dinero
