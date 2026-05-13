#include "rpc/event_bus.h"
#include "common/logger.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>

namespace dinero {
namespace rpc {

// ============================================================================
// EventData implementations
// ============================================================================

din::Json TransactionEventData::toJson() const {
    din::Json j;
    j["event_type"] = event_type_to_string(type);
    j["event_id"] = event_id;
    j["timestamp"] = timestamp;
    j["txid"] = txid;
    j["amount"] = amount;
    j["fee"] = fee;
    j["confirmations"] = static_cast<Json::UInt>(confirmations);

    if (!affected_addresses.empty()) {
        din::Json addrs;
        for (const auto& addr : affected_addresses) {
            addrs.append(addr);
        }
        j["addresses"] = addrs;
    }

    return j;
}

din::Json BlockEventData::toJson() const {
    din::Json j;
    j["event_type"] = event_type_to_string(type);
    j["event_id"] = event_id;
    j["timestamp"] = timestamp;
    j["block_hash"] = block_hash;
    j["height"] = static_cast<Json::UInt>(height);
    j["tx_count"] = static_cast<Json::UInt>(tx_count);
    j["total_amount"] = total_amount;
    if (!miner_address.empty()) {
        j["miner_address"] = miner_address;
    }
    return j;
}

din::Json BalanceEventData::toJson() const {
    din::Json j;
    j["event_type"] = event_type_to_string(type);
    j["event_id"] = event_id;
    j["timestamp"] = timestamp;
    j["address"] = address;
    j["old_balance"] = old_balance;
    j["new_balance"] = new_balance;
    j["delta"] = delta;
    return j;
}

din::Json MempoolEventData::toJson() const {
    din::Json j;
    j["event_type"] = event_type_to_string(type);
    j["event_id"] = event_id;
    j["timestamp"] = timestamp;
    j["tx_count"] = tx_count;
    j["total_size"] = total_size;
    j["min_fee_rate"] = min_fee_rate;
    j["median_fee_rate"] = median_fee_rate;
    j["max_fee_rate"] = max_fee_rate;
    return j;
}

din::Json ChainEventData::toJson() const {
    din::Json j;
    j["event_type"] = event_type_to_string(type);
    j["event_id"] = event_id;
    j["timestamp"] = timestamp;
    j["old_height"] = old_height;
    j["new_height"] = new_height;
    j["tip_hash"] = tip_hash;
    j["sync_progress"] = sync_progress;
    if (blocks_orphaned > 0) {
        j["blocks_orphaned"] = blocks_orphaned;
    }
    return j;
}

// ============================================================================
// EventFilter implementation
// ============================================================================

bool EventFilter::matches(const EventData& event) const {
    // Check event type filter
    if (!event_types.empty()) {
        bool type_match = false;
        for (auto type : event_types) {
            if (event.type == type) {
                type_match = true;
                break;
            }
        }
        if (!type_match) return false;
    }

    // Check address filter (for transaction events)
    if (!addresses.empty()) {
        if (auto* tx_event = dynamic_cast<const TransactionEventData*>(&event)) {
            bool addr_match = false;
            for (const auto& filter_addr : addresses) {
                for (const auto& event_addr : tx_event->affected_addresses) {
                    if (filter_addr == event_addr) {
                        addr_match = true;
                        break;
                    }
                }
                if (addr_match) break;
            }
            if (!addr_match) return false;
        } else if (auto* balance_event = dynamic_cast<const BalanceEventData*>(&event)) {
            bool addr_match = false;
            for (const auto& filter_addr : addresses) {
                if (filter_addr == balance_event->address) {
                    addr_match = true;
                    break;
                }
            }
            if (!addr_match) return false;
        }
    }

    // Check minimum amount filter
    if (min_amount > 0) {
        if (auto* tx_event = dynamic_cast<const TransactionEventData*>(&event)) {
            if (tx_event->amount < min_amount) return false;
        }
    }

    // Check confirmed only filter
    if (confirmed_only) {
        if (auto* tx_event = dynamic_cast<const TransactionEventData*>(&event)) {
            if (tx_event->confirmations == 0) return false;
        }
    }

    return true;
}

// ============================================================================
// EventBus implementation
// ============================================================================

uint64_t EventBus::subscribe(const EventFilter& filter, EventCallback callback) {
    std::lock_guard<std::mutex> lock(get_mutex());

    uint64_t id = next_subscription_id_++;
    auto sub = std::make_shared<Subscription>(id, filter, std::move(callback));
    subscriptions_[id] = sub;

    return id;
}

uint64_t EventBus::subscribe(EventType type, EventCallback callback) {
    EventFilter filter;
    filter.event_types.push_back(type);
    return subscribe(filter, std::move(callback));
}

uint64_t EventBus::subscribe_all(EventCallback callback) {
    EventFilter filter; // Empty filter = match all
    return subscribe(filter, std::move(callback));
}

bool EventBus::unsubscribe(uint64_t subscription_id) {
    std::lock_guard<std::mutex> lock(get_mutex());

    auto it = subscriptions_.find(subscription_id);
    if (it != subscriptions_.end()) {
        subscriptions_.erase(it);
        return true;
    }
    return false;
}

void EventBus::publish(const EventData& event) {
    std::vector<std::shared_ptr<Subscription>> active_subs;

    {
        std::lock_guard<std::mutex> lock(get_mutex());
        events_published_++;
        events_by_type_[event.type]++;

        // Copy active subscriptions
        for (const auto& pair : subscriptions_) {
            if (pair.second->is_active()) {
                active_subs.push_back(pair.second);
            }
        }
    }

    // Notify subscribers outside lock to prevent deadlocks
    for (const auto& sub : active_subs) {
        try {
            sub->notify(event);
            events_delivered_++;
        } catch (const std::exception& e) {
            // Log and continue notifying other subscribers.
            dinero::g_logger.warning("[EventBus] Subscriber callback failed: " + std::string(e.what()));
        }
    }
}

void EventBus::publish_transaction(EventType type, const std::string& txid,
                                   uint64_t amount, uint64_t fee,
                                   const std::vector<std::string>& addresses) {
    TransactionEventData event(type);
    event.event_id = generate_event_id();
    event.timestamp = get_timestamp_ms();
    event.txid = txid;
    event.amount = amount;
    event.fee = fee;
    event.confirmations = (type == EventType::TransactionConfirmed) ? 1 : 0;
    event.affected_addresses = addresses;

    publish(event);
}

void EventBus::publish_block(EventType type, const std::string& block_hash,
                             uint32_t height, uint32_t tx_count) {
    BlockEventData event(type);
    event.event_id = generate_event_id();
    event.timestamp = get_timestamp_ms();
    event.block_hash = block_hash;
    event.height = height;
    event.tx_count = tx_count;

    publish(event);
}

void EventBus::publish_balance_change(const std::string& address,
                                      uint64_t old_balance, uint64_t new_balance) {
    BalanceEventData event(EventType::WalletBalanceChanged);
    event.event_id = generate_event_id();
    event.timestamp = get_timestamp_ms();
    event.address = address;
    event.old_balance = old_balance;
    event.new_balance = new_balance;
    event.delta = static_cast<int64_t>(new_balance) - static_cast<int64_t>(old_balance);

    publish(event);
}

void EventBus::publish_mempool_update(uint32_t tx_count, uint64_t total_size,
                                      double min_fee, double median_fee, double max_fee) {
    MempoolEventData event(EventType::MempoolSizeChanged);
    event.event_id = generate_event_id();
    event.timestamp = get_timestamp_ms();
    event.tx_count = tx_count;
    event.total_size = total_size;
    event.min_fee_rate = min_fee;
    event.median_fee_rate = median_fee;
    event.max_fee_rate = max_fee;

    publish(event);
}

size_t EventBus::subscription_count() const {
    std::lock_guard<std::mutex> lock(get_mutex());
    return subscriptions_.size();
}

void EventBus::clear_subscriptions() {
    std::lock_guard<std::mutex> lock(get_mutex());
    subscriptions_.clear();
}

EventBus::Stats EventBus::get_stats() const {
    std::lock_guard<std::mutex> lock(get_mutex());

    Stats stats;
    stats.total_subscriptions = subscriptions_.size();
    stats.active_subscriptions = 0;
    for (const auto& pair : subscriptions_) {
        if (pair.second->is_active()) {
            stats.active_subscriptions++;
        }
    }
    stats.events_published = events_published_;
    stats.events_delivered = events_delivered_;
    stats.events_by_type = events_by_type_;

    return stats;
}

std::string EventBus::generate_event_id() const {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << dis(gen);
    return oss.str();
}

int64_t EventBus::get_timestamp_ms() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ============================================================================
// Helper functions
// ============================================================================

const char* event_type_to_string(EventType type) {
    switch (type) {
        case EventType::TransactionReceived: return "transaction_received";
        case EventType::TransactionConfirmed: return "transaction_confirmed";
        case EventType::TransactionRejected: return "transaction_rejected";
        case EventType::NewBlock: return "new_block";
        case EventType::BlockOrphaned: return "block_orphaned";
        case EventType::WalletBalanceChanged: return "wallet_balance_changed";
        case EventType::WalletNewAddress: return "wallet_new_address";
        case EventType::WalletIncomingTx: return "wallet_incoming_tx";
        case EventType::WalletOutgoingTx: return "wallet_outgoing_tx";
        case EventType::MempoolSizeChanged: return "mempool_size_changed";
        case EventType::MempoolFeeChanged: return "mempool_fee_changed";
        case EventType::ChainReorg: return "chain_reorg";
        case EventType::ChainSyncing: return "chain_syncing";
        case EventType::ChainSynced: return "chain_synced";
        case EventType::MiningStarted: return "mining_started";
        case EventType::MiningStopped: return "mining_stopped";
        case EventType::MiningBlockFound: return "mining_block_found";
        default: return "unknown";
    }
}

EventType event_type_from_string(const std::string& str) {
    if (str == "transaction_received") return EventType::TransactionReceived;
    if (str == "transaction_confirmed") return EventType::TransactionConfirmed;
    if (str == "transaction_rejected") return EventType::TransactionRejected;
    if (str == "new_block") return EventType::NewBlock;
    if (str == "block_orphaned") return EventType::BlockOrphaned;
    if (str == "wallet_balance_changed") return EventType::WalletBalanceChanged;
    if (str == "wallet_new_address") return EventType::WalletNewAddress;
    if (str == "wallet_incoming_tx") return EventType::WalletIncomingTx;
    if (str == "wallet_outgoing_tx") return EventType::WalletOutgoingTx;
    if (str == "mempool_size_changed") return EventType::MempoolSizeChanged;
    if (str == "mempool_fee_changed") return EventType::MempoolFeeChanged;
    if (str == "chain_reorg") return EventType::ChainReorg;
    if (str == "chain_syncing") return EventType::ChainSyncing;
    if (str == "chain_synced") return EventType::ChainSynced;
    if (str == "mining_started") return EventType::MiningStarted;
    if (str == "mining_stopped") return EventType::MiningStopped;
    if (str == "mining_block_found") return EventType::MiningBlockFound;

    throw std::invalid_argument("Unknown event type: " + str);
}

} // namespace rpc
} // namespace dinero
