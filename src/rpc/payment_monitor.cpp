#include "rpc/payment_monitor.h"
#include "common/logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// PaymentMonitor Implementation
// ═══════════════════════════════════════════════════════════════

PaymentMonitor::PaymentMonitor(EventBus* event_bus, WebSocketServer* ws_server)
    : event_bus_(event_bus)
    , ws_server_(ws_server)
    , last_cleanup_(std::chrono::system_clock::now())
{
    if (!event_bus_) {
        throw std::runtime_error("PaymentMonitor: EventBus is required");
    }

    // Subscribe to relevant events
    EventFilter tx_filter;
    tx_filter.event_types = {EventType::TransactionReceived};
    tx_subscription_id_ = event_bus_->subscribe(
        tx_filter,
        [this](const EventData& data) { this->on_transaction_received(data); }
    );

    EventFilter utxo_filter;
    utxo_filter.event_types = {EventType::TransactionReceived};  // UTXO events come as transactions
    utxo_subscription_id_ = event_bus_->subscribe(
        utxo_filter,
        [this](const EventData& data) { this->on_utxo_added(data); }
    );

    EventFilter block_filter;
    block_filter.event_types = {EventType::NewBlock};
    block_subscription_id_ = event_bus_->subscribe(
        block_filter,
        [this](const EventData& data) { this->on_block_confirmed(data); }
    );

    dinero::g_logger.info("[PaymentMonitor] Initialized with EventBus subscriptions");
}

PaymentMonitor::~PaymentMonitor() {
    // Subscriptions are automatically cleaned up by EventBus
    // when subscription IDs go out of scope
}

std::string PaymentMonitor::generate_watch_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t random = dis(gen);

    std::ostringstream oss;
    oss << "watch_" << std::hex << std::setw(16) << std::setfill('0') << random;
    return oss.str();
}

std::string PaymentMonitor::watch_address(const WatchConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate address
    if (config.address.empty()) {
        throw std::invalid_argument("Address cannot be empty");
    }

    // Generate watch ID
    std::string watch_id = generate_watch_id();

    // Create watch state
    WatchState state;
    state.watch_id = watch_id;
    state.config = config;
    state.created_at = std::chrono::system_clock::now();
    state.active = true;

    // Store watch
    watches_[watch_id] = state;

    // Index by address for fast lookup
    address_index_[config.address].push_back(watch_id);

    dinero::g_logger.info("[PaymentMonitor] Started watching address: " + config.address +
                          " (watch_id: " + watch_id + ")");

    return watch_id;
}

bool PaymentMonitor::unwatch(const std::string& watch_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = watches_.find(watch_id);
    if (it == watches_.end()) {
        return false;
    }

    std::string address = it->second.config.address;

    // Remove from watches
    watches_.erase(it);

    // Remove from address index
    auto& watch_list = address_index_[address];
    watch_list.erase(
        std::remove(watch_list.begin(), watch_list.end(), watch_id),
        watch_list.end()
    );

    if (watch_list.empty()) {
        address_index_.erase(address);
    }

    dinero::g_logger.info("[PaymentMonitor] Stopped watching: " + watch_id);

    return true;
}

std::optional<WatchState> PaymentMonitor::get_watch_status(const std::string& watch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = watches_.find(watch_id);
    if (it != watches_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<PaymentEvent> PaymentMonitor::get_payments(
    const std::string& address,
    uint64_t since_timestamp) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PaymentEvent> result;

    // Find all watches for this address
    auto addr_it = address_index_.find(address);
    if (addr_it == address_index_.end()) {
        return result;
    }

    // Collect payments from all watches
    for (const auto& watch_id : addr_it->second) {
        auto watch_it = watches_.find(watch_id);
        if (watch_it == watches_.end()) {
            continue;
        }

        for (const auto& payment : watch_it->second.detected_payments) {
            // Filter by timestamp if specified
            if (since_timestamp > 0) {
                auto payment_time = std::chrono::duration_cast<std::chrono::seconds>(
                    payment.detected_at.time_since_epoch()
                ).count();

                if (static_cast<uint64_t>(payment_time) < since_timestamp) {
                    continue;
                }
            }

            result.push_back(payment);
        }
    }

    // Sort by detection time (newest first)
    std::sort(result.begin(), result.end(),
              [](const PaymentEvent& a, const PaymentEvent& b) {
                  return a.detected_at > b.detected_at;
              });

    return result;
}

std::vector<std::string> PaymentMonitor::find_matching_watches(const std::string& address) const {
    // Must be called with mutex already locked

    auto it = address_index_.find(address);
    if (it != address_index_.end()) {
        return it->second;
    }

    return {};
}

RiskLevel PaymentMonitor::assess_risk(
    const std::string& txid,
    bool& rbf_enabled,
    bool& double_spend_detected,
    double& fee_rate,
    uint32_t& peer_count) const
{
    (void)txid;

    // Initialize outputs
    rbf_enabled = false;
    double_spend_detected = false;
    fee_rate = 0.0;
    peer_count = 0;

    // Determine risk level
    if (double_spend_detected) {
        return RiskLevel::HIGH;
    }

    if (rbf_enabled) {
        return RiskLevel::HIGH;
    }

    // No direct mempool/network telemetry is wired into PaymentMonitor yet.
    // Treat unknown fee/network confidence as medium risk.
    if (fee_rate <= 0.0 || peer_count == 0) {
        return RiskLevel::MEDIUM;
    }

    if (fee_rate < 1.0) {
        return RiskLevel::MEDIUM;
    }

    if (peer_count < 4) {
        return RiskLevel::MEDIUM;
    }

    return RiskLevel::LOW;
}

PaymentEvent PaymentMonitor::analyze_transaction(const std::string& txid) const {
    PaymentEvent event;
    event.txid = txid;
    event.address = "";
    event.amount_una = 0;
    event.confirmations = 0;
    event.detected_at = std::chrono::system_clock::now();
    event.rbf_enabled = false;
    event.double_spend_detected = false;
    event.fee_rate_una_per_byte = 0.0;
    event.peer_count = 0;
    event.risk = RiskLevel::MEDIUM;

    // Prefer real observed state from tracked payments for this txid.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        uint32_t best_confirmations = 0;

        for (const auto& watch_pair : watches_) {
            const auto& watch = watch_pair.second;
            for (const auto& payment : watch.detected_payments) {
                if (payment.txid != txid) {
                    continue;
                }
                if (!found || payment.confirmations >= best_confirmations) {
                    event = payment;
                    best_confirmations = payment.confirmations;
                    found = true;
                }
            }
        }

        if (!found) {
            event.txid = txid;
            event.detected_at = std::chrono::system_clock::now();
        }
    }

    // If not yet confirmed, refresh risk from currently available telemetry.
    if (event.confirmations == 0) {
        event.risk = assess_risk(
            txid,
            event.rbf_enabled,
            event.double_spend_detected,
            event.fee_rate_una_per_byte,
            event.peer_count
        );
    }

    // If confirmed on-chain, risk is none
    if (event.confirmations > 0) {
        event.risk = RiskLevel::NONE;
    }

    return event;
}

void PaymentMonitor::on_transaction_received(const EventData& event_data) {
    // Cast to TransactionEventData
    const auto* tx_event = dynamic_cast<const TransactionEventData*>(&event_data);
    if (!tx_event) {
        return;
    }

    std::string txid = tx_event->txid;
    const auto& addresses = tx_event->affected_addresses;

    std::lock_guard<std::mutex> lock(mutex_);

    // Check each output address
    for (const auto& address : addresses) {

        // Find matching watches
        auto watch_ids = find_matching_watches(address);

        for (const auto& watch_id : watch_ids) {
            auto& watch = watches_[watch_id];

            if (!watch.active) {
                continue;
            }

            // Create payment event
            PaymentEvent payment;
            payment.txid = txid;
            payment.address = address;
            payment.confirmations = tx_event->confirmations;
            payment.detected_at = std::chrono::system_clock::now();

            // Extract amount from transaction event
            payment.amount_una = tx_event->amount;

            // Assess risk
            payment.risk = assess_risk(
                txid,
                payment.rbf_enabled,
                payment.double_spend_detected,
                payment.fee_rate_una_per_byte,
                payment.peer_count
            );
            if (payment.confirmations > 0) {
                payment.risk = RiskLevel::NONE;
            }

            // Check if amount matches expectation (if specified)
            if (watch.config.expected_amount_una > 0) {
                if (payment.amount_una != watch.config.expected_amount_una) {
                    dinero::g_logger.warning(
                        "[PaymentMonitor] Amount mismatch for watch " + watch_id +
                        ": expected " + std::to_string(watch.config.expected_amount_una) +
                        ", got " + std::to_string(payment.amount_una)
                    );
                    continue;  // Skip this payment
                }
            }

            // Record payment
            watch.detected_payments.push_back(payment);

            dinero::g_logger.info(
                "[PaymentMonitor] Payment detected: " + txid +
                " to " + address +
                " (watch: " + watch_id + ")" +
                " risk: " + std::to_string(static_cast<int>(payment.risk))
            );

            // Send notifications
            if (!watch.config.client_id.empty() && ws_server_) {
                send_payment_event(watch.config.client_id, "payment.detected", payment, watch_id);
            }

            if (!watch.config.webhook_url.empty()) {
                send_webhook(watch.config.webhook_url, "payment.detected", payment);
            }

            // Auto-unwatch if configured
            if (watch.config.auto_unwatch_after_payment) {
                watch.active = false;
                dinero::g_logger.info("[PaymentMonitor] Auto-unwatched: " + watch_id);
            }
        }
    }
}

void PaymentMonitor::on_utxo_added(const EventData& event_data) {
    // Similar to on_transaction_received, but for UTXO-specific events
    on_transaction_received(event_data);
}

void PaymentMonitor::on_block_confirmed(const EventData& event_data) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update confirmation counts for all tracked payments
    for (auto& watch_pair : watches_) {
        auto& watch = watch_pair.second;

        for (auto& payment : watch.detected_payments) {
            // Increment confirmations
            payment.confirmations++;

            // Update risk level (confirmed = no risk)
            if (payment.confirmations >= watch.config.min_confirmations) {
                payment.risk = RiskLevel::NONE;

                // Send confirmation event
                if (!watch.config.client_id.empty() && ws_server_) {
                    send_payment_event(watch.config.client_id, "payment.confirmed", payment, watch.watch_id);
                }

                if (!watch.config.webhook_url.empty()) {
                    send_webhook(watch.config.webhook_url, "payment.confirmed", payment);
                }
            }
        }
    }

    // Periodic cleanup
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - last_cleanup_).count();

    if (elapsed >= 10) {  // Cleanup every 10 minutes
        cleanup_watches();
        last_cleanup_ = now;
    }
}

void PaymentMonitor::send_payment_event(
    const std::string& client_id,
    const std::string& event_type,
    const PaymentEvent& payment,
    const std::string& watch_id)
{
    if (!ws_server_) {
        return;
    }

    din::Json event = din::obj();
    event["jsonrpc"] = "2.0";
    event["method"] = event_type;

    din::Json params = din::obj();
    params["watch_id"] = watch_id;
    params["txid"] = payment.txid;
    params["address"] = payment.address;
    params["amount_una"] = static_cast<Json::UInt64>(payment.amount_una);
    params["confirmations"] = payment.confirmations;

    // Risk level
    std::string risk_str;
    switch (payment.risk) {
        case RiskLevel::NONE:   risk_str = "none"; break;
        case RiskLevel::LOW:    risk_str = "low"; break;
        case RiskLevel::MEDIUM: risk_str = "medium"; break;
        case RiskLevel::HIGH:   risk_str = "high"; break;
    }
    params["risk"] = risk_str;

    // Risk factors
    params["rbf_enabled"] = payment.rbf_enabled;
    params["double_spend_detected"] = payment.double_spend_detected;
    params["fee_rate_una_per_byte"] = payment.fee_rate_una_per_byte;

    // Timestamp
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        payment.detected_at.time_since_epoch()
    ).count();
    params["detected_at"] = static_cast<Json::Int64>(timestamp);

    event["params"] = params;

    ws_server_->send_to_client(client_id, event);
}

void PaymentMonitor::send_webhook(
    const std::string& webhook_url,
    const std::string& event_type,
    const PaymentEvent& payment)
{
    // Webhook transport is not wired in this build.
    dinero::g_logger.warning("[PaymentMonitor] Webhook delivery unavailable; skipped url=" + webhook_url +
                             " event=" + event_type +
                             " txid=" + payment.txid);
}

void PaymentMonitor::cleanup_watches() {
    // Remove inactive watches older than 24 hours
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24);

    std::vector<std::string> to_remove;

    for (const auto& pair : watches_) {
        const auto& watch = pair.second;

        if (!watch.active && watch.created_at < cutoff) {
            to_remove.push_back(pair.first);
        }
    }

    for (const auto& watch_id : to_remove) {
        unwatch(watch_id);
    }

    if (!to_remove.empty()) {
        dinero::g_logger.info("[PaymentMonitor] Cleaned up " +
                              std::to_string(to_remove.size()) + " inactive watches");
    }
}

size_t PaymentMonitor::get_active_watch_count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto& pair : watches_) {
        if (pair.second.active) {
            count++;
        }
    }

    return count;
}

std::vector<WatchState> PaymentMonitor::get_all_watches() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<WatchState> result;
    result.reserve(watches_.size());

    for (const auto& pair : watches_) {
        result.push_back(pair.second);
    }

    return result;
}

} // namespace rpc
} // namespace dinero
