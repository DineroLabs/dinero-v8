#include "economic_safety_oracle.h"
#include <set>

namespace dinero {
namespace economic {
namespace test {

std::vector<NodeID> EconomicSafetyOracle::getAllNodes(const EconomicTrace& trace) const {
    std::set<NodeID> unique_nodes(trace.nodes.begin(), trace.nodes.end());
    return std::vector<NodeID>(unique_nodes.begin(), unique_nodes.end());
}

std::vector<EconomicEvent> EconomicSafetyOracle::getEventsByType(
    const EconomicTrace& trace,
    EconomicEventType type
) const {
    std::vector<EconomicEvent> filtered;
    for (const auto& event : trace.events) {
        if (event.type == type) {
            filtered.push_back(event);
        }
    }
    return filtered;
}

std::vector<EconomicEvent> EconomicSafetyOracle::getEventsForTx(
    const EconomicTrace& trace,
    const TxID& tx_id
) const {
    std::vector<EconomicEvent> tx_events;
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id) {
            tx_events.push_back(event);
        }
    }
    return tx_events;
}

bool EconomicSafetyOracle::wasTxAccepted(const EconomicTrace& trace, const TxID& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.tx_id && *event.tx_id == tx_id &&
            event.success) {
            return true;
        }
    }
    return false;
}

bool EconomicSafetyOracle::wasTxRejected(const EconomicTrace& trace, const TxID& tx_id) const {
    for (const auto& event : trace.events) {
        if ((event.type == EconomicEventType::TX_REJECTED_LOW_FEE ||
             event.type == EconomicEventType::TX_REJECTED_DUST ||
             event.type == EconomicEventType::TX_REJECTED_INVALID) &&
            event.tx_id && *event.tx_id == tx_id &&
            !event.success) {
            return true;
        }
    }
    return false;
}

bool EconomicSafetyOracle::wasTxConfirmed(const EconomicTrace& trace, const TxID& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }
    return false;
}

std::optional<uint64_t> EconomicSafetyOracle::getTxFee(
    const EconomicTrace& trace,
    const TxID& tx_id
) const {
    // Look for TX_ACCEPTED_TO_MEMPOOL or TX_INCLUDED_IN_BLOCK event with fee
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id && event.fee_una) {
            return event.fee_una;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>> EconomicSafetyOracle::getTxValues(
    const EconomicTrace& trace,
    const TxID& tx_id
) const {
    // Look for event with input/output values
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id &&
            event.input_value && event.output_value) {
            return std::make_pair(*event.input_value, *event.output_value);
        }
    }
    return std::nullopt;
}

} // namespace test
} // namespace economic
} // namespace dinero
