#include "mempool_simulator.h"
#include <algorithm>
#include <numeric>

namespace dinero {
namespace economic {
namespace test {

MempoolSimulator::MempoolSimulator(
    const NodeID& node_id,
    const EconomicPolicy& policy,
    std::shared_ptr<FeeEstimator> fee_estimator
)
    : node_id_(node_id)
    , policy_(policy)
    , fee_estimator_(fee_estimator)
    , event_sequence_(0)
{
}

EconomicEvent MempoolSimulator::submitTransaction(const MempoolEntry& entry, uint64_t timestamp) {
    // Validate fees
    auto fee_error = validateFees(entry);
    if (fee_error) {
        return createEvent(
            EconomicEventType::TX_REJECTED_LOW_FEE,
            timestamp,
            false,
            *fee_error,
            entry.tx_id,
            entry
        );
    }

    // Check for conflicts (same tx_id already in mempool)
    if (hasTx(entry.tx_id)) {
        return createEvent(
            EconomicEventType::TX_REJECTED_INVALID,
            timestamp,
            false,
            "Transaction already in mempool",
            entry.tx_id,
            entry
        );
    }

    // Check if mempool has space
    if (!hasSpace(entry.tx_size_bytes)) {
        // Try to evict low-fee transactions
        auto eviction_events = evictLowFeeTxs(entry.tx_size_bytes, timestamp);

        // Check if eviction made enough space
        if (!hasSpace(entry.tx_size_bytes)) {
            return createEvent(
                EconomicEventType::TX_REJECTED_INVALID,
                timestamp,
                false,
                "Mempool full, eviction failed",
                entry.tx_id,
                entry
            );
        }
    }

    // Add to mempool
    mempool_[entry.tx_id] = entry;

    return createEvent(
        EconomicEventType::TX_ACCEPTED_TO_MEMPOOL,
        timestamp,
        true,
        "",
        entry.tx_id,
        entry
    );
}

EconomicEvent MempoolSimulator::replaceTransaction(
    const TxID& old_tx_id,
    const MempoolEntry& new_entry,
    uint64_t timestamp
) {
    // Check if old transaction exists
    auto old_entry = getTx(old_tx_id);
    if (!old_entry) {
        return createEvent(
            EconomicEventType::TX_REJECTED_INVALID,
            timestamp,
            false,
            "Original transaction not in mempool",
            new_entry.tx_id,
            new_entry
        );
    }

    // Validate RBF rules
    auto rbf_error = validateRBF(*old_entry, new_entry);
    if (rbf_error) {
        return createEvent(
            EconomicEventType::TX_REJECTED_INVALID,
            timestamp,
            false,
            *rbf_error,
            new_entry.tx_id,
            new_entry
        );
    }

    // Remove old transaction
    mempool_.erase(old_tx_id);

    // Add new transaction
    mempool_[new_entry.tx_id] = new_entry;

    // Create RBF event
    auto event = createEvent(
        EconomicEventType::TX_REPLACED_RBF,
        timestamp,
        true,
        "",
        new_entry.tx_id,
        new_entry
    );
    event.replaced_tx_id = old_tx_id;
    event.fee_delta = new_entry.fee_una - old_entry->fee_una;

    return event;
}

bool MempoolSimulator::hasTx(const TxID& tx_id) const {
    return mempool_.find(tx_id) != mempool_.end();
}

std::optional<MempoolEntry> MempoolSimulator::getTx(const TxID& tx_id) const {
    auto it = mempool_.find(tx_id);
    if (it != mempool_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<MempoolEntry> MempoolSimulator::getAllTxs() const {
    std::vector<MempoolEntry> txs;
    txs.reserve(mempool_.size());
    for (const auto& [tx_id, entry] : mempool_) {
        txs.push_back(entry);
    }
    return txs;
}

std::vector<MempoolEntry> MempoolSimulator::getTxsSortedByFeeRate() const {
    auto txs = getAllTxs();
    std::sort(txs.begin(), txs.end(), [](const MempoolEntry& a, const MempoolEntry& b) {
        return a.fee_rate > b.fee_rate;  // Descending order
    });
    return txs;
}

uint64_t MempoolSimulator::getSize() const {
    return getCurrentSize();
}

uint64_t MempoolSimulator::getTotalFees() const {
    uint64_t total = 0;
    for (const auto& [tx_id, entry] : mempool_) {
        total += entry.fee_una;
    }
    return total;
}

std::optional<EconomicEvent> MempoolSimulator::removeTx(const TxID& tx_id, uint64_t timestamp) {
    auto it = mempool_.find(tx_id);
    if (it == mempool_.end()) {
        return std::nullopt;
    }

    auto entry = it->second;
    mempool_.erase(it);

    return createEvent(
        EconomicEventType::TX_INCLUDED_IN_BLOCK,  // Assume removal is due to confirmation
        timestamp,
        true,
        "",
        tx_id,
        entry
    );
}

std::vector<EconomicEvent> MempoolSimulator::removeTxs(const std::vector<TxID>& tx_ids, uint64_t timestamp) {
    std::vector<EconomicEvent> events;
    for (const auto& tx_id : tx_ids) {
        auto event = removeTx(tx_id, timestamp);
        if (event) {
            events.push_back(*event);
        }
    }
    return events;
}

std::vector<EconomicEvent> MempoolSimulator::evictLowFeeTxs(uint64_t required_bytes, uint64_t timestamp) {
    std::vector<EconomicEvent> eviction_events;

    // Get transactions sorted by fee rate (lowest first)
    auto txs = getTxsSortedByFeeRate();
    std::reverse(txs.begin(), txs.end());  // Lowest fee rate first

    uint64_t freed_bytes = 0;

    for (const auto& entry : txs) {
        if (freed_bytes >= required_bytes) {
            break;
        }

        // Evict this transaction
        mempool_.erase(entry.tx_id);
        freed_bytes += entry.tx_size_bytes;

        auto event = createEvent(
            EconomicEventType::TX_EVICTED_MEMPOOL,
            timestamp,
            true,
            "Evicted to make space",
            entry.tx_id,
            entry
        );
        eviction_events.push_back(event);
    }

    return eviction_events;
}

void MempoolSimulator::clear() {
    mempool_.clear();
}

std::optional<std::string> MempoolSimulator::validateFees(const MempoolEntry& entry) const {
    // Check value conservation: output_value <= input_value
    if (entry.output_value > entry.input_value) {
        return "Outputs exceed inputs";
    }

    // Calculate fee
    uint64_t fee = entry.input_value - entry.output_value;
    if (fee != entry.fee_una) {
        return "Fee mismatch";
    }

    // Check minimum relay fee
    if (fee < policy_.min_relay_fee_una) {
        return "Fee below minimum relay fee";
    }

    // Check dust threshold (simplified: just check if any output is below dust)
    // In a real implementation, we'd check actual outputs
    // For simulation, we assume dust check is done elsewhere

    // Check fee rate
    double calculated_fee_rate = static_cast<double>(fee) / static_cast<double>(entry.tx_size_bytes);
    double min_relay_rate = static_cast<double>(policy_.min_relay_fee_una) / 1000.0;  // Assume 1KB min tx

    if (calculated_fee_rate < min_relay_rate) {
        return "Fee rate too low";
    }

    // Check for fee overflow (fee should be reasonable)
    // Maximum reasonable fee: 1 BTC = 100,000,000 una
    if (fee > 100000000) {
        return "Fee unreasonably high (possible overflow)";
    }

    return std::nullopt;  // Valid
}

std::optional<std::string> MempoolSimulator::validateRBF(
    const MempoolEntry& old_entry,
    const MempoolEntry& new_entry
) const {
    if (!policy_.enable_rbf) {
        return "RBF not enabled";
    }

    // Check that new fee rate is higher
    if (new_entry.fee_rate <= old_entry.fee_rate) {
        return "New fee rate must be higher";
    }

    // Check that fee rate increased by minimum increment
    if (new_entry.fee_rate < old_entry.fee_rate + policy_.rbf_min_fee_increment) {
        return "Fee rate increase below minimum";
    }

    // Check that absolute fee increased by minimum amount
    if (new_entry.fee_una < old_entry.fee_una + policy_.rbf_min_absolute_fee) {
        return "Absolute fee increase below minimum";
    }

    return std::nullopt;  // Valid
}

EconomicState MempoolSimulator::captureState(uint64_t timestamp, uint32_t chain_height) const {
    EconomicState state;
    state.node_id = node_id_;
    state.timestamp = timestamp;

    // Mempool state
    state.mempool_entries = getAllTxs();
    state.mempool_tx_count = mempool_.size();
    state.mempool_size_bytes = getCurrentSize();
    state.mempool_total_fees = getTotalFees();

    // Chain state
    state.chain_height = chain_height;
    state.chain_tip_hash = "";  // Will be filled by simulator

    // Policy state
    state.min_relay_fee_una = policy_.min_relay_fee_una;
    state.dust_threshold_una = policy_.dust_threshold_una;

    // Fee estimates
    if (fee_estimator_) {
        state.fee_estimates = fee_estimator_->getAllEstimates();
    }

    // Economic metrics (lifetime)
    state.total_fees_collected = 0;  // Will be tracked by simulator
    state.total_txs_confirmed = 0;   // Will be tracked by simulator
    state.total_txs_rejected = 0;    // Will be tracked by simulator

    // Attack detection
    state.spam_tx_count = 0;          // Will be tracked by simulator
    state.free_relay_attempts = 0;    // Will be tracked by simulator

    return state;
}

void MempoolSimulator::setMinRelayFee(uint64_t fee_una) {
    policy_.min_relay_fee_una = fee_una;
}

void MempoolSimulator::setDustThreshold(uint64_t threshold_una) {
    policy_.dust_threshold_una = threshold_una;
}

bool MempoolSimulator::hasSpace(uint32_t tx_size_bytes) const {
    return (getCurrentSize() + tx_size_bytes) <= policy_.max_mempool_size_bytes &&
           mempool_.size() < policy_.max_mempool_tx_count;
}

uint64_t MempoolSimulator::getCurrentSize() const {
    uint64_t total_size = 0;
    for (const auto& [tx_id, entry] : mempool_) {
        total_size += entry.tx_size_bytes;
    }
    return total_size;
}

EconomicEvent MempoolSimulator::createEvent(
    EconomicEventType type,
    uint64_t timestamp,
    bool success,
    const std::string& error_message,
    const std::optional<TxID>& tx_id,
    const std::optional<MempoolEntry>& entry
) {
    EconomicEvent event;
    event.type = type;
    event.timestamp = timestamp;
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.success = success;
    event.error_message = error_message;

    if (tx_id) {
        event.tx_id = *tx_id;
    }

    if (entry) {
        event.fee_una = entry->fee_una;
        event.input_value = entry->input_value;
        event.output_value = entry->output_value;
        event.tx_size_bytes = entry->tx_size_bytes;
        event.fee_rate = entry->fee_rate;
    }

    // Mempool stats
    event.mempool_tx_count = mempool_.size();
    event.mempool_size_bytes = getCurrentSize();
    event.mempool_total_fees = getTotalFees();

    return event;
}

} // namespace test
} // namespace economic
} // namespace dinero
