#pragma once

#include "lightning_types.h"
#include "gossip_types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <optional>
// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN

namespace dinero {
namespace lightning {

// Watchtower appointment - encrypted penalty data for a channel
struct WatchtowerAppointment {
    // Appointment identifier (hash of breach transaction ID)
    std::array<uint8_t, 32> locator;

    // Encrypted penalty transaction (AES-256)
    // Decryption key derived from breach transaction data
    std::vector<uint8_t> encrypted_blob;

    // Channel information
    std::array<uint8_t, 32> channel_id;
    ShortChannelID short_channel_id;

    // Client node ID
    NodeID client_node_id;

    // Appointment metadata
    uint32_t start_block;        // Start monitoring from this block
    uint32_t end_block;          // Stop monitoring at this block
    uint32_t created_at;         // Timestamp of appointment creation

    // Payment for watchtower service (in una)
    uint64_t reward_una;

    // Status
    enum class Status : uint8_t {
        PENDING,       // Waiting for start_block
        ACTIVE,        // Currently monitoring
        TRIGGERED,     // Breach detected, penalty sent
        EXPIRED,       // end_block reached, no breach
        CANCELLED      // Client cancelled appointment
    };
    Status status;

    // Breach information (if triggered)
    std::optional<std::array<uint8_t, 32>> breach_txid;
    std::optional<uint32_t> breach_block_height;
    std::optional<std::array<uint8_t, 32>> penalty_txid;
};

// Watchtower info advertised via node announcement
struct WatchtowerInfo {
    NodeID watchtower_node_id;
    std::string alias;

    // Service parameters
    uint64_t base_fee_una;           // Flat fee per appointment
    uint32_t proportional_fee_ppm;        // Proportional fee (parts per million)
    uint32_t max_appointments;            // Maximum concurrent appointments
    uint32_t blocks_per_appointment;      // Default monitoring duration

    // Service features
    bool supports_encrypted_blobs;
    bool supports_zero_knowledge;         // Privacy-preserving protocol
    bool supports_altruistic;             // Free for small channels

    // Network addresses
    std::vector<NodeAnnouncement::Address> addresses;

    // Statistics
    uint32_t num_active_appointments;
    uint32_t num_triggered_appointments;
    uint32_t uptime_percentage;           // 0-100
    uint32_t avg_response_time_seconds;

    // Last update
    uint32_t last_updated;
};

// Breach detection event
struct BreachEvent {
    std::array<uint8_t, 32> channel_id;
    std::array<uint8_t, 32> breach_txid;
    uint32_t block_height;
    uint64_t block_timestamp;

    // Commitment info
    uint64_t commitment_number;
    std::array<uint8_t, 32> revocation_secret;

    // Parties involved
    NodeID victim_node_id;
    NodeID cheater_node_id;

    // Channel state
    uint64_t victim_balance_sat;
    uint64_t cheater_balance_sat;
};

// Penalty transaction result
struct PenaltyResult {
    std::array<uint8_t, 32> penalty_txid;
    uint32_t block_height;
    uint64_t amount_recovered_sat;
    uint64_t watchtower_fee_una;
    bool confirmed;
};

// Watchtower service statistics
struct WatchtowerStats {
    // Service info
    uint32_t num_clients;
    uint32_t num_active_appointments;
    uint32_t num_expired_appointments;
    uint32_t num_triggered_appointments;

    // Performance
    uint64_t total_monitored_blocks;
    uint64_t total_scanned_transactions;
    uint32_t avg_scan_time_ms;

    // Financial
    uint64_t total_fees_earned_sat;
    uint64_t total_penalties_sent_sat;
    uint64_t total_rewards_paid_sat;

    // Uptime
    uint64_t service_started;  // Phase 8.5: Unix timestamp (deterministic from block height)
    uint32_t uptime_seconds;
    uint32_t num_restarts;
};

// Client appointment request
struct AppointmentRequest {
    std::array<uint8_t, 32> channel_id;
    std::array<uint8_t, 32> locator;
    std::vector<uint8_t> encrypted_blob;

    uint32_t start_block;
    uint32_t end_block;

    // Payment
    uint64_t offered_fee_una;

    // Optional: signed authorization from client
    std::optional<std::vector<uint8_t>> client_signature;
};

// Watchtower response to appointment request
struct AppointmentResponse {
    bool accepted;
    std::string reason;  // If rejected

    // If accepted
    std::optional<std::array<uint8_t, 32>> appointment_id;
    std::optional<uint64_t> required_fee_sat;
    std::optional<uint32_t> monitoring_duration_blocks;
};

// Breach notification to client
struct BreachNotification {
    std::array<uint8_t, 32> channel_id;
    std::array<uint8_t, 32> appointment_id;
    std::array<uint8_t, 32> breach_txid;
    std::array<uint8_t, 32> penalty_txid;

    uint32_t breach_block_height;
    uint64_t penalty_amount_sat;
    uint64_t watchtower_fee_una;

    bool penalty_confirmed;
};

// Appointment cancellation request
struct AppointmentCancellation {
    std::array<uint8_t, 32> appointment_id;
    NodeID client_node_id;
    std::vector<uint8_t> signature;  // Proves client authorization

    bool request_refund;
    uint64_t refund_amount_sat;
};

} // namespace lightning
} // namespace dinero
