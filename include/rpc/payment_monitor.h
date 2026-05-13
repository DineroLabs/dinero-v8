#pragma once

#include "din_json.h"
#include "rpc/event_bus.h"
#include "rpc/websocket_server.h"
#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>

namespace dinero {
namespace rpc {

/**
 * Risk Level for Zero-Confirmation Payments
 */
enum class RiskLevel {
    NONE,      // Confirmed on-chain (1+ confirmations)
    LOW,       // Safe for instant acceptance (good fee, no double-spend)
    MEDIUM,    // Caution advised (low fee, limited network view)
    HIGH       // Dangerous (RBF enabled, double-spend detected)
};

/**
 * Payment Detection Event
 */
struct PaymentEvent {
    std::string txid;
    std::string address;
    uint64_t amount_una;
    uint32_t confirmations;
    RiskLevel risk;
    std::chrono::system_clock::time_point detected_at;

    // Risk factors
    bool rbf_enabled;
    bool double_spend_detected;
    double fee_rate_una_per_byte;
    uint32_t peer_count;
};

/**
 * Watch Configuration
 */
struct WatchConfig {
    std::string address;
    uint64_t expected_amount_una;  // 0 = any amount
    std::string webhook_url;            // Optional HTTP callback
    std::string client_id;              // WebSocket connection ID
    uint32_t min_confirmations;         // Trigger confirmed event after N blocks
    bool auto_unwatch_after_payment;    // Remove watch after first payment

    WatchConfig()
        : expected_amount_una(0)
        , min_confirmations(1)
        , auto_unwatch_after_payment(false)
    {}
};

/**
 * Active Watch State
 */
struct WatchState {
    std::string watch_id;
    WatchConfig config;
    std::chrono::system_clock::time_point created_at;
    std::vector<PaymentEvent> detected_payments;
    bool active;
};

/**
 * Payment Monitor
 *
 * Provides real-time payment detection for merchant applications.
 * Uses EventBus to listen for incoming transactions and UTXOs,
 * sends instant notifications via WebSocket when payments arrive.
 *
 * Features:
 * - Instant mempool detection (<50ms)
 * - Risk assessment for zero-conf acceptance
 * - Confirmation tracking
 * - WebSocket + webhook notifications
 * - Automatic watch cleanup
 */
class PaymentMonitor {
public:
    /**
     * Constructor
     * @param event_bus EventBus for transaction notifications
     * @param ws_server WebSocket server for real-time events
     */
    PaymentMonitor(EventBus* event_bus, WebSocketServer* ws_server);
    ~PaymentMonitor();

    /**
     * Start watching an address for payments
     * @param config Watch configuration
     * @return Watch ID for tracking
     */
    std::string watch_address(const WatchConfig& config);

    /**
     * Stop watching an address
     * @param watch_id Watch ID from watch_address()
     * @return true if watch was removed
     */
    bool unwatch(const std::string& watch_id);

    /**
     * Get current status of watched address
     * @param watch_id Watch ID
     * @return Watch state with detected payments
     */
    std::optional<WatchState> get_watch_status(const std::string& watch_id) const;

    /**
     * Get all payments received at an address
     * @param address Dinero address
     * @param since_timestamp Only show payments after this time (0 = all)
     * @return List of payment events
     */
    std::vector<PaymentEvent> get_payments(
        const std::string& address,
        uint64_t since_timestamp = 0
    ) const;

    /**
     * Analyze risk level for a transaction
     * @param txid Transaction ID
     * @return Payment event with risk assessment
     */
    PaymentEvent analyze_transaction(const std::string& txid) const;

    /**
     * Get count of active watches
     */
    size_t get_active_watch_count() const;

    /**
     * Get all active watches (for admin/debugging)
     */
    std::vector<WatchState> get_all_watches() const;

private:
    /**
     * EventBus callback: New transaction received
     */
    void on_transaction_received(const EventData& event_data);

    /**
     * EventBus callback: New UTXO added to wallet
     */
    void on_utxo_added(const EventData& event_data);

    /**
     * EventBus callback: Block confirmed
     */
    void on_block_confirmed(const EventData& event_data);

    /**
     * Check if transaction matches any watched addresses
     * @return List of matching watch IDs
     */
    std::vector<std::string> find_matching_watches(const std::string& address) const;

    /**
     * Assess risk level for zero-confirmation transaction
     */
    RiskLevel assess_risk(
        const std::string& txid,
        bool& rbf_enabled,
        bool& double_spend_detected,
        double& fee_rate,
        uint32_t& peer_count
    ) const;

    /**
     * Send WebSocket event to client
     */
    void send_payment_event(
        const std::string& client_id,
        const std::string& event_type,
        const PaymentEvent& payment,
        const std::string& watch_id
    );

    /**
     * Send HTTP webhook notification
     */
    void send_webhook(
        const std::string& webhook_url,
        const std::string& event_type,
        const PaymentEvent& payment
    );

    /**
     * Generate unique watch ID
     */
    std::string generate_watch_id() const;

    /**
     * Cleanup expired/inactive watches
     */
    void cleanup_watches();

    // Dependencies
    EventBus* event_bus_;
    WebSocketServer* ws_server_;

    // State
    mutable std::mutex mutex_;
    std::map<std::string, WatchState> watches_;            // watch_id -> state
    std::map<std::string, std::vector<std::string>> address_index_;  // address -> watch_ids

    // EventBus subscription IDs
    std::string tx_subscription_id_;
    std::string utxo_subscription_id_;
    std::string block_subscription_id_;

    // Cleanup timer
    std::chrono::system_clock::time_point last_cleanup_;
};

} // namespace rpc
} // namespace dinero
