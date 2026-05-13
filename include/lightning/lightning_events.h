#pragma once

#include "din_json.h"
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace dinero {
namespace lightning {

/**
 * @enum LightningEventType
 * @brief Types of Lightning Network events
 *
 * Phase 14: Live Lightning Event Stream
 *
 * Event categories:
 * - Watchtower events (breach detection, justice transactions)
 * - Channel lifecycle events (open, close, update)
 * - Payment events (sent, received, failed)
 * - Routing events (route discovery, failures)
 */
enum class LightningEventType {
    // Watchtower Events (Phase 10-12)
    WATCHTOWER_BREACH_DETECTED,      // Counterparty broadcast old state
    WATCHTOWER_JUSTICE_BROADCAST,    // Justice transaction sent
    WATCHTOWER_JUSTICE_CONFIRMED,    // Justice transaction confirmed

    // Channel Lifecycle Events (Phase 13)
    CHANNEL_OPENED,                  // Channel successfully opened
    CHANNEL_STATE_UPDATE,            // Channel state changed
    CHANNEL_COOPERATIVE_CLOSE,       // Mutual shutdown initiated
    CHANNEL_FORCE_CLOSE,             // Unilateral close initiated
    CHANNEL_CLOSED,                  // Channel fully closed

    // Sweep Events (Phase 13.4)
    CHANNEL_SWEEP_SCHEDULED,         // CSV sweep scheduled
    CHANNEL_SWEEP_EXECUTED,          // Sweep transaction broadcast

    // Payment Events (Phase 8-9)
    PAYMENT_SENT,                    // Outgoing payment succeeded
    PAYMENT_RECEIVED,                // Incoming payment received
    PAYMENT_FAILED,                  // Payment attempt failed
    PAYMENT_HTLC_FORWARDED,          // HTLC forwarded (routing node)

    // Routing Events (Phase 9)
    ROUTE_DISCOVERY_SUCCESS,         // Found route to destination
    ROUTE_DISCOVERY_FAILED,          // No route found
    ROUTE_CHANNEL_FAILURE,           // Channel failed during routing

    // HTLC Events (Phase 8)
    HTLC_ADDED,                      // New HTLC added
    HTLC_SETTLED,                    // HTLC settled with preimage
    HTLC_FAILED,                     // HTLC failed/timed out

    // Peer Events
    PEER_CONNECTED,                  // New peer connected
    PEER_DISCONNECTED,               // Peer disconnected

    // System Events
    LIGHTNING_SERVICE_STARTED,       // Lightning service initialized
    LIGHTNING_SERVICE_STOPPED,       // Lightning service stopped
};

/**
 * @struct LightningEvent
 * @brief Base structure for Lightning Network events
 *
 * All Lightning events share:
 * - Unique event ID
 * - Event type
 * - Timestamp
 * - Associated channel (if applicable)
 * - JSON payload with event-specific data
 *
 * Thread Safety: Events are immutable after creation
 */
struct LightningEvent {
    std::string event_id;              // Unique event identifier (UUID)
    LightningEventType event_type;     // Type of event
    uint64_t timestamp;                // Unix timestamp (milliseconds)
    std::string channel_id;            // Associated channel (empty if N/A)
    din::Json payload;                 // Event-specific data

    LightningEvent()
        : event_type(LightningEventType::LIGHTNING_SERVICE_STARTED),
          timestamp(0) {}

    /**
     * @brief Convert event to JSON for WebSocket transmission
     * @return din::Json JSON representation
     */
    din::Json toJson() const;

    /**
     * @brief Get human-readable event type name
     * @return std::string Event type as string
     */
    std::string getEventTypeName() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Event Builder Helper Functions
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Create a watchtower breach detected event
 */
LightningEvent createBreachDetectedEvent(
    const std::string& channel_id,
    uint64_t commitment_number,
    const std::string& breach_txid
);

/**
 * @brief Create a justice transaction broadcast event
 */
LightningEvent createJusticeBroadcastEvent(
    const std::string& channel_id,
    const std::string& justice_txid,
    uint64_t penalty_amount_una
);

/**
 * @brief Create a channel state update event
 */
LightningEvent createChannelStateUpdateEvent(
    const std::string& channel_id,
    const std::string& old_state,
    const std::string& new_state,
    uint64_t local_balance_muna,
    uint64_t remote_balance_muna
);

/**
 * @brief Create a channel cooperative close event
 */
LightningEvent createChannelCooperativeCloseEvent(
    const std::string& channel_id,
    const std::string& closing_txid
);

/**
 * @brief Create a channel force close event
 */
LightningEvent createChannelForceCloseEvent(
    const std::string& channel_id,
    const std::string& commitment_txid,
    uint32_t csv_delay
);

/**
 * @brief Create a channel sweep scheduled event
 */
LightningEvent createChannelSweepScheduledEvent(
    const std::string& channel_id,
    uint64_t expiry_height,
    uint64_t amount_una
);

/**
 * @brief Create a channel sweep executed event
 */
LightningEvent createChannelSweepExecutedEvent(
    const std::string& channel_id,
    const std::string& sweep_txid,
    uint64_t amount_una
);

/**
 * @brief Create a payment sent event
 */
LightningEvent createPaymentSentEvent(
    const std::string& channel_id,
    const std::string& payment_hash,
    uint64_t amount_muna,
    uint64_t fee_muna
);

/**
 * @brief Create a payment received event
 */
LightningEvent createPaymentReceivedEvent(
    const std::string& channel_id,
    const std::string& payment_hash,
    uint64_t amount_muna
);

/**
 * @brief Create a payment failed event
 */
LightningEvent createPaymentFailedEvent(
    const std::string& channel_id,
    const std::string& payment_hash,
    const std::string& failure_reason
);

/**
 * @brief Create a route discovery failed event
 */
LightningEvent createRouteDiscoveryFailedEvent(
    const std::string& destination_node_id,
    const std::string& failure_reason
);

/**
 * @brief Create an HTLC added event
 */
LightningEvent createHTLCAddedEvent(
    const std::string& channel_id,
    uint64_t htlc_id,
    uint64_t amount_muna,
    const std::string& payment_hash
);

/**
 * @brief Create an HTLC settled event
 */
LightningEvent createHTLCSettledEvent(
    const std::string& channel_id,
    uint64_t htlc_id,
    const std::string& preimage
);

/**
 * @brief Create an HTLC failed event
 */
LightningEvent createHTLCFailedEvent(
    const std::string& channel_id,
    uint64_t htlc_id,
    const std::string& failure_reason
);

} // namespace lightning
} // namespace dinero
