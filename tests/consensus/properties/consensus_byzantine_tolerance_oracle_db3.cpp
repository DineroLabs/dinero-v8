#include "consensus_byzantine_tolerance_oracle_db3.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<ByzantineViolation> DB3Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<ByzantineViolation> violations;

    // Find all confirmed transactions
    auto confirmed_txs = getConfirmedTransactions(trace);
    if (confirmed_txs.empty()) {
        // No confirmed transactions - property trivially holds
        return violations;
    }

    // Find all conflicting transaction sets
    auto conflict_sets = findConflictingSets(trace);
    if (conflict_sets.empty()) {
        // No conflicts detected - property trivially holds
        return violations;
    }

    // Check each conflict set for multiple confirmations
    std::unordered_set<std::string> checked;  // Avoid duplicate reports
    for (const auto& [tx_hash, conflicting_txs] : conflict_sets) {
        if (checked.count(tx_hash)) {
            continue;  // Already processed this conflict set
        }

        // Check if this transaction was confirmed
        if (confirmed_txs.count(tx_hash) == 0) {
            continue;  // This tx not confirmed, skip
        }

        // Check if any conflicting transaction was also confirmed
        for (const auto& conflict_tx : conflicting_txs) {
            if (confirmed_txs.count(conflict_tx)) {
                // Violation: Both transactions in conflict set were confirmed
                std::ostringstream desc;
                desc << "Double-spend succeeded: Conflicting transactions "
                     << tx_hash.substr(0, 8) << "... and "
                     << conflict_tx.substr(0, 8) << "... both confirmed.";

                ByzantineViolation v(
                    getName(),
                    desc.str(),
                    trace.end_time
                );

                v.details = "Both: " + tx_hash + " and " + conflict_tx;

                // Find which nodes confirmed each transaction
                for (const auto& snapshot : trace.snapshots) {
                    if (wasTransactionConfirmed(trace, tx_hash, snapshot.node_id)) {
                        v.involved_nodes.push_back(snapshot.node_id + "(tx1)");
                    }
                    if (wasTransactionConfirmed(trace, conflict_tx, snapshot.node_id)) {
                        v.involved_nodes.push_back(snapshot.node_id + "(tx2)");
                    }
                }

                violations.push_back(v);

                // Mark both as checked to avoid duplicate reports
                checked.insert(tx_hash);
                checked.insert(conflict_tx);
                break;  // Only report first violation in this conflict set
            }
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::unordered_set<std::string> DB3Oracle::getConfirmedTransactions(
    const ConsensusTrace& trace
) const {
    std::unordered_set<std::string> confirmed;

    // Look for TX_ACCEPTED events (transactions accepted into mempool/blocks)
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::TX_ACCEPTED && event.success) {
            if (event.tx_id && !event.tx_id->empty()) {
                confirmed.insert(*event.tx_id);
            }
        }
    }

    return confirmed;
}

std::unordered_map<std::string, std::unordered_set<std::string>>
DB3Oracle::findConflictingSets(const ConsensusTrace& trace) const {
    std::unordered_map<std::string, std::unordered_set<std::string>> conflicts;

    // Look for DOUBLE_SPEND_ATTEMPT actions
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::DOUBLE_SPEND_ATTEMPT) {
            // Double-spend attempt indicates conflicting transactions
            // The tx_id in the action represents one tx in the conflict
            if (action.tx_id && !action.tx_id->empty()) {
                // For now, we track that this tx is part of a double-spend
                // In a full implementation, we'd need to track both conflicting txs
                // For Phase 5e, we'll detect conflicts by looking for TX_REJECTED
                // events with error messages indicating double-spend
            }
        }
    }

    // Alternative: Look for TX_REJECTED events indicating conflicts
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::TX_REJECTED &&
            !event.success &&
            event.error_message.find("conflict") != std::string::npos) {

            if (event.tx_id && !event.tx_id->empty()) {
                // This transaction conflicted with another
                // Mark it as part of a conflict set
                // (We'll need more sophisticated tracking for full implementation)
            }
        }
    }

    return conflicts;
}

bool DB3Oracle::wasTransactionConfirmed(
    const ConsensusTrace& trace,
    const std::string& tx_hash,
    const NodeID& node_id
) const {
    // Check if this node accepted the transaction
    for (const auto& event : trace.events) {
        if (event.node_id == node_id &&
            event.type == ConsensusEventType::TX_ACCEPTED &&
            event.tx_id && *event.tx_id == tx_hash &&
            event.success) {
            return true;
        }
    }
    return false;
}

} // namespace test
} // namespace consensus
} // namespace dinero
