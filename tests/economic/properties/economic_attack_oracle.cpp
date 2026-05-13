#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<std::string> EconomicAttackOracle::getConfirmedTxs(const EconomicTrace& trace) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::vector<std::string> EconomicAttackOracle::getReorgedTxs(const EconomicTrace& trace) const {
    std::vector<std::string> txs;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_REORGED_OUT && event.tx_id) {
            txs.push_back(*event.tx_id);
        }
    }
    return txs;
}

std::optional<uint64_t> EconomicAttackOracle::getTxConfirmedTime(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return event.timestamp;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> EconomicAttackOracle::getTxConfirmedHeight(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id && event.block_height) {
            return *event.block_height;
        }
    }
    return std::nullopt;
}

bool EconomicAttackOracle::wasTxReorgedOut(const EconomicTrace& trace, const std::string& tx_id) const {
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_REORGED_OUT &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }
    return false;
}

std::vector<EconomicEvent> EconomicAttackOracle::getEventsByType(const EconomicTrace& trace, EconomicEventType type) const {
    std::vector<EconomicEvent> events;
    for (const auto& event : trace.events) {
        if (event.type == type) {
            events.push_back(event);
        }
    }
    return events;
}

std::vector<EconomicEvent> EconomicAttackOracle::getEventsForTx(const EconomicTrace& trace, const std::string& tx_id) const {
    std::vector<EconomicEvent> events;
    for (const auto& event : trace.events) {
        if (event.tx_id && *event.tx_id == tx_id) {
            events.push_back(event);
        }
    }
    return events;
}

std::vector<EconomicEvent> EconomicAttackOracle::getBlockTemplateEvents(const EconomicTrace& trace) const {
    return getEventsByType(trace, EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED);
}

bool EconomicAttackOracle::areConflicting(const std::string& tx1_id, const std::string& tx2_id) const {
    // Simplified: Two transactions are conflicting if they share a naming pattern
    // indicating they are different versions of the same transaction
    // e.g., "tx1_v1" and "tx1_v2" are conflicting
    // or "tx_spend_a" and "tx_spend_b" (both spending same input)

    // For testing purposes, we'll use a simple heuristic:
    // Transactions with "_v1", "_v2" suffixes or "_a", "_b" suffixes are conflicting
    // This is a simplified model for the test framework

    // Extract base name (before underscore suffix)
    auto getBaseName = [](const std::string& tx_id) -> std::string {
        size_t pos = tx_id.find_last_of('_');
        if (pos != std::string::npos) {
            std::string suffix = tx_id.substr(pos + 1);
            // Check if suffix is v1, v2, a, b, etc.
            if (suffix.length() <= 2 &&
                (suffix[0] == 'v' || suffix[0] == 'a' || suffix[0] == 'b')) {
                return tx_id.substr(0, pos);
            }
        }
        return tx_id;
    };

    return getBaseName(tx1_id) == getBaseName(tx2_id) && tx1_id != tx2_id;
}

} // namespace test
} // namespace economic
} // namespace dinero
