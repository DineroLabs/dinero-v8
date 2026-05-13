#include "economic_liveness_oracle.h"
#include <set>

namespace dinero {
namespace economic {
namespace test {

std::vector<NodeID> EconomicLivenessOracle::getAllNodes(const EconomicTrace& trace) const {
    std::set<NodeID> unique_nodes(trace.nodes.begin(), trace.nodes.end());
    return std::vector<NodeID>(unique_nodes.begin(), unique_nodes.end());
}

std::vector<EconomicEvent> EconomicLivenessOracle::getEventsByType(
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

std::vector<EconomicEvent> EconomicLivenessOracle::getEventsForTx(
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

bool EconomicLivenessOracle::wasTxAccepted(const EconomicTrace& trace, const TxID& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.tx_id && *event.tx_id == tx_id &&
            event.success) {
            return true;
        }
    }
    return false;
}

bool EconomicLivenessOracle::wasTxConfirmed(const EconomicTrace& trace, const TxID& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }
    return false;
}

bool EconomicLivenessOracle::wasTxInTemplate(const EconomicTrace& trace, const TxID& tx_id) const {
    // Check TX_SELECTED_FOR_BLOCK events
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_SELECTED_FOR_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }

    // Also check BLOCK_TEMPLATE_ASSEMBLED events with template_txs
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED &&
            event.template_txs) {
            for (const auto& template_tx_id : *event.template_txs) {
                if (template_tx_id == tx_id) {
                    return true;
                }
            }
        }
    }

    return false;
}

std::optional<uint64_t> EconomicLivenessOracle::getTxAcceptedTime(
    const EconomicTrace& trace,
    const TxID& tx_id
) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.tx_id && *event.tx_id == tx_id &&
            event.success) {
            return event.timestamp;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> EconomicLivenessOracle::getTxConfirmedTime(
    const EconomicTrace& trace,
    const TxID& tx_id
) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return event.timestamp;
        }
    }
    return std::nullopt;
}

std::vector<TxID> EconomicLivenessOracle::getUnconfirmedTxs(const EconomicTrace& trace) const {
    std::set<TxID> accepted_txs;
    std::set<TxID> confirmed_txs;

    // Collect accepted transactions
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.tx_id && event.success) {
            accepted_txs.insert(*event.tx_id);
        }
    }

    // Collect confirmed transactions
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK && event.tx_id) {
            confirmed_txs.insert(*event.tx_id);
        }
    }

    // Return accepted but not confirmed
    std::vector<TxID> unconfirmed;
    for (const auto& tx_id : accepted_txs) {
        if (confirmed_txs.find(tx_id) == confirmed_txs.end()) {
            unconfirmed.push_back(tx_id);
        }
    }

    return unconfirmed;
}

} // namespace test
} // namespace economic
} // namespace dinero
