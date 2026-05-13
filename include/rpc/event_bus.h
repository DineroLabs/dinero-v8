#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <memory>
#include "din_json.h"

namespace dinero {
namespace rpc {

/**
 * Event types that can be published through the event bus
 */
enum class EventType {
    // Transaction events
    TransactionReceived,        // New transaction entered mempool
    TransactionConfirmed,       // Transaction included in block
    TransactionRejected,        // Transaction rejected by mempool

    // Block events
    NewBlock,                   // New block added to chain
    BlockOrphaned,              // Block became orphaned (reorg)

    // Wallet events
    WalletBalanceChanged,       // Wallet balance updated
    WalletNewAddress,           // New address generated
    WalletIncomingTx,           // Incoming transaction to wallet
    WalletOutgoingTx,           // Outgoing transaction from wallet

    // Mempool events
    MempoolSizeChanged,         // Mempool size changed significantly
    MempoolFeeChanged,          // Mempool fee estimates changed

    // Chain events
    ChainReorg,                 // Blockchain reorganization
    ChainSyncing,               // Chain sync progress update
    ChainSynced,                // Chain fully synchronized

    // Mining events
    MiningStarted,              // Mining operation started
    MiningStopped,              // Mining operation stopped
    MiningBlockFound,           // Miner found new block
};

/**
 * Base class for all event data
 */
struct EventData {
    EventType type;
    int64_t timestamp;          // Unix timestamp in milliseconds
    std::string event_id;       // Unique event identifier

    virtual ~EventData() = default;
    virtual din::Json toJson() const = 0;

    EventData(EventType t) : type(t), timestamp(0), event_id("") {}
};

/**
 * Transaction event data
 */
struct TransactionEventData : public EventData {
    std::string txid;
    uint64_t amount;            // In una
    uint64_t fee;               // In una
    uint32_t confirmations;
    std::vector<std::string> affected_addresses;

    TransactionEventData(EventType t) : EventData(t), amount(0), fee(0), confirmations(0) {}

    din::Json toJson() const override;
};

/**
 * Block event data
 */
struct BlockEventData : public EventData {
    std::string block_hash;
    uint32_t height;
    uint32_t tx_count;
    uint64_t total_amount;
    std::string miner_address;

    BlockEventData(EventType t) : EventData(t), height(0), tx_count(0), total_amount(0) {}

    din::Json toJson() const override;
};

/**
 * Balance event data
 */
struct BalanceEventData : public EventData {
    std::string address;
    uint64_t old_balance;
    uint64_t new_balance;
    int64_t delta;

    BalanceEventData(EventType t) : EventData(t), old_balance(0), new_balance(0), delta(0) {}

    din::Json toJson() const override;
};

/**
 * Mempool event data
 */
struct MempoolEventData : public EventData {
    uint32_t tx_count;
    uint64_t total_size;        // In bytes
    double min_fee_rate;        // Sat/vB
    double median_fee_rate;
    double max_fee_rate;

    MempoolEventData(EventType t) : EventData(t), tx_count(0), total_size(0),
                                     min_fee_rate(0.0), median_fee_rate(0.0), max_fee_rate(0.0) {}

    din::Json toJson() const override;
};

/**
 * Chain event data
 */
struct ChainEventData : public EventData {
    uint32_t old_height;
    uint32_t new_height;
    uint32_t blocks_orphaned;
    std::string tip_hash;
    double sync_progress;       // 0.0 to 1.0

    ChainEventData(EventType t) : EventData(t), old_height(0), new_height(0),
                                   blocks_orphaned(0), sync_progress(0.0) {}

    din::Json toJson() const override;
};

/**
 * Event subscriber callback type
 */
using EventCallback = std::function<void(const EventData&)>;

/**
 * Event filter for selective subscription
 */
struct EventFilter {
    std::vector<EventType> event_types;      // Empty = subscribe to all
    std::vector<std::string> addresses;      // Filter by address (for tx events)
    uint64_t min_amount = 0;                 // Filter by minimum amount
    bool confirmed_only = false;             // Only confirmed transactions

    bool matches(const EventData& event) const;
};

/**
 * Subscription handle for managing event subscriptions
 */
class Subscription {
public:
    Subscription(uint64_t id, const EventFilter& filter, EventCallback callback)
        : id_(id), filter_(filter), callback_(std::move(callback)), active_(true) {}

    uint64_t id() const { return id_; }
    const EventFilter& filter() const { return filter_; }
    bool is_active() const { return active_; }
    void set_active(bool active) { active_ = active; }

    void notify(const EventData& event) const {
        if (active_ && filter_.matches(event)) {
            callback_(event);
        }
    }

private:
    uint64_t id_;
    EventFilter filter_;
    EventCallback callback_;
    bool active_;
};

/**
 * Central event bus for publishing and subscribing to blockchain events
 *
 * Thread-safe publish/subscribe pattern for real-time event notifications.
 * Integrates with WebSocket server to push events to connected clients.
 */
class EventBus {
public:
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    // Disable copy/move
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /**
     * Subscribe to events with optional filter
     * Returns subscription ID for later unsubscribe
     */
    uint64_t subscribe(const EventFilter& filter, EventCallback callback);

    /**
     * Subscribe to specific event type
     */
    uint64_t subscribe(EventType type, EventCallback callback);

    /**
     * Subscribe to all events
     */
    uint64_t subscribe_all(EventCallback callback);

    /**
     * Unsubscribe from events
     */
    bool unsubscribe(uint64_t subscription_id);

    /**
     * Publish event to all matching subscribers
     */
    void publish(const EventData& event);

    /**
     * Publish transaction event
     */
    void publish_transaction(EventType type, const std::string& txid,
                            uint64_t amount, uint64_t fee,
                            const std::vector<std::string>& addresses = {});

    /**
     * Publish block event
     */
    void publish_block(EventType type, const std::string& block_hash,
                      uint32_t height, uint32_t tx_count);

    /**
     * Publish balance change event
     */
    void publish_balance_change(const std::string& address,
                               uint64_t old_balance, uint64_t new_balance);

    /**
     * Publish mempool event
     */
    void publish_mempool_update(uint32_t tx_count, uint64_t total_size,
                               double min_fee, double median_fee, double max_fee);

    /**
     * Get subscription count
     */
    size_t subscription_count() const;

    /**
     * Clear all subscriptions
     */
    void clear_subscriptions();

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_subscriptions = 0;
        uint64_t active_subscriptions = 0;
        uint64_t events_published = 0;
        uint64_t events_delivered = 0;
        std::unordered_map<EventType, uint64_t> events_by_type;
    };

    Stats get_stats() const;

    /**
     * Get current timestamp in milliseconds
     */
    int64_t get_timestamp_ms() const;

private:
    EventBus() : next_subscription_id_(1), events_published_(0), events_delivered_(0) {}

    std::string generate_event_id() const;

    // Use function-local static for mutex to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    std::unordered_map<uint64_t, std::shared_ptr<Subscription>> subscriptions_;
    uint64_t next_subscription_id_;

    // Statistics
    mutable uint64_t events_published_;
    mutable uint64_t events_delivered_;
    mutable std::unordered_map<EventType, uint64_t> events_by_type_;
};

/**
 * Helper to convert EventType to string
 */
const char* event_type_to_string(EventType type);

/**
 * Helper to parse EventType from string
 */
EventType event_type_from_string(const std::string& str);

} // namespace rpc
} // namespace dinero
