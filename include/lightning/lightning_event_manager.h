#pragma once

#include "lightning/lightning_events.h"
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>

namespace dinero {
namespace lightning {

/**
 * @class LightningEventManager
 * @brief Central event aggregation and distribution system for Lightning Network
 *
 * Phase 14: Live Lightning Event Stream (WebSocket)
 *
 * Responsibilities:
 * - Aggregate events from all Lightning components
 * - Manage event subscriptions (WebSocket clients, RPC callbacks, etc.)
 * - Distribute events to all active subscribers
 * - Maintain event history buffer for late subscribers
 * - Provide event filtering by type and channel
 *
 * Architecture:
 * - Producer-Consumer pattern with lock-free queue
 * - Multiple subscriber support (WebSocket, RPC, GUI)
 * - Event replay for new subscribers
 * - Thread-safe event emission
 *
 * Thread Safety: All public methods are thread-safe
 */
class LightningEventManager {
public:
    /**
     * @brief Event callback function type
     *
     * Subscribers provide a callback that will be invoked for each event.
     * The callback receives a const reference to the event.
     */
    using EventCallback = std::function<void(const LightningEvent&)>;

    /**
     * @brief Subscription handle for managing subscriptions
     *
     * Each subscription has a unique ID.
     * Destroying the handle automatically unsubscribes.
     */
    class SubscriptionHandle {
    public:
        SubscriptionHandle(uint64_t id, LightningEventManager* manager)
            : m_id(id), m_manager(manager) {}

        ~SubscriptionHandle() {
            if (m_manager) {
                m_manager->unsubscribe(m_id);
            }
        }

        // Non-copyable, movable
        SubscriptionHandle(const SubscriptionHandle&) = delete;
        SubscriptionHandle& operator=(const SubscriptionHandle&) = delete;
        SubscriptionHandle(SubscriptionHandle&& other) noexcept
            : m_id(other.m_id), m_manager(other.m_manager) {
            other.m_manager = nullptr;
        }

        uint64_t getId() const { return m_id; }

    private:
        uint64_t m_id;
        LightningEventManager* m_manager;
    };

    /**
     * @brief Construct LightningEventManager
     * @param history_size Maximum number of events to keep in history buffer
     */
    explicit LightningEventManager(size_t history_size = 1000);
    ~LightningEventManager();

    // ═══════════════════════════════════════════════════════════════════════════
    // Event Emission (called by Lightning components)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Emit a Lightning event
     *
     * Thread-safe method for Lightning components to emit events.
     * Events are queued and distributed asynchronously to all subscribers.
     *
     * @param event The event to emit
     */
    void emitEvent(const LightningEvent& event);

    /**
     * @brief Emit multiple events in batch
     *
     * More efficient than calling emitEvent() multiple times.
     *
     * @param events Vector of events to emit
     */
    void emitEvents(const std::vector<LightningEvent>& events);

    // ═══════════════════════════════════════════════════════════════════════════
    // Subscription Management (called by RPC, WebSocket, GUI)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Subscribe to Lightning events
     *
     * Creates a new subscription with the given callback.
     * The callback will be invoked for all future events.
     *
     * @param callback Function to call for each event
     * @param replay_history If true, replay recent events to the new subscriber
     * @return std::unique_ptr<SubscriptionHandle> Handle for managing the subscription
     */
    std::unique_ptr<SubscriptionHandle> subscribe(
        EventCallback callback,
        bool replay_history = false
    );

    /**
     * @brief Subscribe with event type filter
     *
     * Only events matching the specified types will be delivered.
     *
     * @param callback Function to call for each event
     * @param event_types Vector of event types to subscribe to
     * @param replay_history If true, replay recent matching events
     * @return std::unique_ptr<SubscriptionHandle> Handle for managing the subscription
     */
    std::unique_ptr<SubscriptionHandle> subscribeFiltered(
        EventCallback callback,
        const std::vector<LightningEventType>& event_types,
        bool replay_history = false
    );

    /**
     * @brief Subscribe to events for a specific channel
     *
     * Only events associated with the specified channel will be delivered.
     *
     * @param callback Function to call for each event
     * @param channel_id Channel ID to filter by
     * @param replay_history If true, replay recent matching events
     * @return std::unique_ptr<SubscriptionHandle> Handle for managing the subscription
     */
    std::unique_ptr<SubscriptionHandle> subscribeChannel(
        EventCallback callback,
        const std::string& channel_id,
        bool replay_history = false
    );

    /**
     * @brief Unsubscribe from events
     *
     * @param subscription_id ID of the subscription to remove
     */
    void unsubscribe(uint64_t subscription_id);

    // ═══════════════════════════════════════════════════════════════════════════
    // Event History Query
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get recent event history
     *
     * @param max_events Maximum number of events to return (0 = all)
     * @return std::vector<LightningEvent> Recent events (newest first)
     */
    std::vector<LightningEvent> getRecentEvents(size_t max_events = 100) const;

    /**
     * @brief Get events for a specific channel
     *
     * @param channel_id Channel ID to filter by
     * @param max_events Maximum number of events to return
     * @return std::vector<LightningEvent> Channel events (newest first)
     */
    std::vector<LightningEvent> getChannelEvents(
        const std::string& channel_id,
        size_t max_events = 100
    ) const;

    /**
     * @brief Get events by type
     *
     * @param event_type Event type to filter by
     * @param max_events Maximum number of events to return
     * @return std::vector<LightningEvent> Matching events (newest first)
     */
    std::vector<LightningEvent> getEventsByType(
        LightningEventType event_type,
        size_t max_events = 100
    ) const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get event manager statistics
     *
     * @return din::Json Statistics including subscriber count, event counts, etc.
     */
    din::Json getStats() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Structures
    // ═══════════════════════════════════════════════════════════════════════════

    struct Subscriber {
        uint64_t id;
        EventCallback callback;
        std::vector<LightningEventType> filter_types;  // Empty = no filter
        std::string filter_channel;                     // Empty = no filter
        bool has_type_filter;
        bool has_channel_filter;

        Subscriber()
            : id(0),
              has_type_filter(false),
              has_channel_filter(false) {}
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    size_t m_history_size;                           // Max events in history buffer
    std::vector<LightningEvent> m_event_history;     // Circular buffer of recent events
    size_t m_history_write_pos;                      // Write position in circular buffer

    std::vector<Subscriber> m_subscribers;           // Active subscriptions
    uint64_t m_next_subscription_id;                 // Next subscription ID

    mutable std::mutex m_mutex;                      // Protects all internal state

    std::atomic<uint64_t> m_total_events_emitted{0};    // Lifetime event counter
    std::atomic<uint64_t> m_total_events_delivered{0};  // Total deliveries to subscribers

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Add event to history buffer
     */
    void addToHistory(const LightningEvent& event);

    /**
     * @brief Deliver event to all matching subscribers
     */
    void deliverEvent(const LightningEvent& event);

    /**
     * @brief Check if event matches subscriber's filter
     */
    bool matchesFilter(const LightningEvent& event, const Subscriber& subscriber) const;

    /**
     * @brief Replay history to a specific subscriber
     */
    void replayHistory(const Subscriber& subscriber);
};

} // namespace lightning
} // namespace dinero
