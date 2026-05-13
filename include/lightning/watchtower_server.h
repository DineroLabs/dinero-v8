#pragma once

#include "lightning/watchtower_types.h"
#include "result.h"
#include <memory>
#include <string>
#include <functional>
#include <array>

namespace lightning {
    class ITimeOracle;  // Phase 8.5: Forward declaration
}

namespace dinero {
namespace lightning {

// Forward declarations
class BreachDetector;
class AppointmentStore;

/**
 * @class WatchtowerServer
 * @brief Remote watchtower service for monitoring client channels (Phase 10+)
 *
 * Provides privacy-preserving watchtower service using encrypted blobs and zero-knowledge proofs.
 * Monitors blockchain for breach attempts and broadcasts penalty transactions on behalf of clients.
 *
 * Phase 8.5 Compliance:
 * - NO background threads (event-driven via onNewBlock callbacks)
 * - NO wall-clock time (uses ITimeOracle for deterministic timestamps)
 * - Purely reactive to blockchain events
 */
class WatchtowerServer {
public:
    // Configuration
    struct Config {
        std::string db_path;
        uint32_t max_appointments = 10000;
        uint64_t max_storage_bytes = 100 * 1024 * 1024;  // 100 MB
        uint64_t base_fee_una = 1000;
        uint32_t proportional_fee_ppm = 100;  // 0.01%
        uint32_t default_monitoring_blocks = 144;  // ~1 day
        bool enable_zero_knowledge = true;
        bool enable_altruistic_mode = false;
    };

    /**
     * @brief Construct WatchtowerServer
     * @param config Server configuration
     * @param our_node_id Our watchtower node ID
     * @param time_oracle Deterministic time source (Phase 8.5)
     */
    WatchtowerServer(const Config& config, const NodeID& our_node_id, ::lightning::ITimeOracle* time_oracle);
    ~WatchtowerServer();

    // Service lifecycle
    Result<void> start();
    void stop();
    bool is_running() const;

    // Appointment management
    Result<AppointmentResponse> accept_appointment(const AppointmentRequest& request);
    Result<void> cancel_appointment(const AppointmentCancellation& cancellation);
    std::vector<WatchtowerAppointment> get_active_appointments() const;
    std::optional<WatchtowerAppointment> get_appointment(const std::array<uint8_t, 32>& appointment_id) const;

    // Breach monitoring (Phase 8.5: Event-driven, called by daemon on new blocks)
    Result<void> process_new_block(uint32_t block_height);
    Result<void> scan_block_for_breaches(uint32_t block_height);

    // Service info
    WatchtowerInfo get_service_info() const;
    WatchtowerStats get_stats() const;
    uint64_t get_storage_usage_bytes() const;
    uint32_t get_num_clients() const;

    // Maintenance
    Result<void> prune_expired_appointments();

    // Callbacks
    using BreachDetectedCallback = std::function<void(const BreachEvent&, const PenaltyResult&)>;
    void set_breach_detected_callback(BreachDetectedCallback callback);

private:
    // Private implementation (Pimpl idiom)
    class Impl;
    std::unique_ptr<Impl> impl_;

    // Helper methods
    bool is_revoked_commitment(const std::array<uint8_t, 32>& txid, const std::array<uint8_t, 32>& locator) const;
    std::optional<WatchtowerAppointment> find_matching_appointment(const std::array<uint8_t, 32>& txid) const;
    std::vector<uint8_t> decrypt_penalty_tx(const std::vector<uint8_t>& encrypted_blob, const std::array<uint8_t, 32>& breach_txid) const;
    Result<std::array<uint8_t, 32>> broadcast_penalty_tx(const std::vector<uint8_t>& penalty_tx);
    Result<PenaltyResult> handle_breach(const BreachEvent& breach, const WatchtowerAppointment& appointment);
    uint64_t calculate_fee(uint32_t monitoring_blocks, uint64_t channel_capacity_sat) const;
    bool should_accept_appointment(const AppointmentRequest& request) const;
};

/**
 * @class BreachDetector
 * @brief Scans blocks for breach attempts using appointment locators
 */
class BreachDetector {
public:
    explicit BreachDetector(std::shared_ptr<WatchtowerServer> server);
    ~BreachDetector();

    Result<void> start_monitoring();
    void stop_monitoring();

    Result<std::vector<BreachEvent>> scan_block(uint32_t block_height);

    Result<void> add_locator(const std::array<uint8_t, 32>& locator, const std::array<uint8_t, 32>& appointment_id);
    Result<void> remove_locator(const std::array<uint8_t, 32>& locator);

    struct DetectorStats {
        uint64_t blocks_scanned;
        uint64_t transactions_scanned;
        uint64_t breaches_detected;
        uint64_t false_positives;
        uint32_t avg_scan_time_ms;
    };
    DetectorStats get_stats() const;

private:
    class LocatorIndex;
    std::shared_ptr<WatchtowerServer> server_;
    std::unique_ptr<LocatorIndex> locator_index_;
};

/**
 * @class AppointmentStore
 * @brief Persistent storage for watchtower appointments
 */
class AppointmentStore {
public:
    explicit AppointmentStore(const std::string& db_path);
    ~AppointmentStore();

    Result<void> store_appointment(const WatchtowerAppointment& appointment);
    Result<void> update_appointment(const WatchtowerAppointment& appointment);
    std::optional<WatchtowerAppointment> get_appointment(const std::array<uint8_t, 32>& appointment_id) const;
    std::vector<WatchtowerAppointment> get_active_appointments() const;

    uint32_t get_num_appointments() const;
    uint32_t get_num_clients() const;
    size_t prune_expired(uint32_t current_block);
    uint64_t get_storage_size() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lightning
} // namespace dinero
