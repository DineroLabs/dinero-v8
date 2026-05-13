#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<std::string> EconomicIncentiveOracle::getAcceptedTxs(const EconomicTrace& trace) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.success && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::vector<std::string> EconomicIncentiveOracle::getConfirmedTxs(const EconomicTrace& trace) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::vector<std::string> EconomicIncentiveOracle::getEvictedTxs(const EconomicTrace& trace) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_EVICTED_MEMPOOL && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::optional<double> EconomicIncentiveOracle::getTxFeeRate(const EconomicTrace& trace, const std::string& tx_id) const {
    // Check all events for this transaction
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id && event.fee_rate) {
            return *event.fee_rate;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> EconomicIncentiveOracle::getTxAcceptedTime(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.success && event.tx_id && *event.tx_id == tx_id) {
            return event.timestamp;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> EconomicIncentiveOracle::getTxConfirmedTime(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return event.timestamp;
        }
    }
    return std::nullopt;
}

std::vector<std::string> EconomicIncentiveOracle::getSelectedTxsAtTime(const EconomicTrace& trace, uint64_t timestamp) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_SELECTED_FOR_BLOCK &&
            event.timestamp == timestamp && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::vector<std::string> EconomicIncentiveOracle::getExcludedTxsAtTime(const EconomicTrace& trace, uint64_t timestamp) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_EXCLUDED_FROM_BLOCK &&
            event.timestamp == timestamp && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

bool EconomicIncentiveOracle::wasTxEvicted(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_EVICTED_MEMPOOL &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }
    return false;
}

std::vector<EconomicEvent> EconomicIncentiveOracle::getEventsByType(const EconomicTrace& trace, EconomicEventType type) const {
    std::vector<EconomicEvent> events;
    for (const auto& event : trace.events) {
        if (event.type == type) {
            events.push_back(event);
        }
    }
    return events;
}

std::vector<EconomicEvent> EconomicIncentiveOracle::getEventsForTx(const EconomicTrace& trace, const std::string& tx_id) const {
    std::vector<EconomicEvent> events;
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id) {
            events.push_back(event);
        }
    }
    return events;
}

} // namespace test
} // namespace economic
} // namespace dinero
