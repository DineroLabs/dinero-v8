#include "consensus_liveness_oracle_dl4.h"
#include <sstream>
#include <set>

namespace dinero {
namespace consensus {
namespace test {

std::vector<LivenessViolation> DL4Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<LivenessViolation> violations;

    // Get all transactions from trace
    auto all_txs = getAllTransactions(trace);

    if (all_txs.empty()) {
        // No transactions to check
        return violations;
    }

    // Check inclusion for each transaction
    for (const auto& tx_hash : all_txs) {
        auto broadcast_time = getTxBroadcastTime(trace, tx_hash);

        if (!broadcast_time) {
            // Transaction wasn't broadcast (shouldn't happen)
            continue;
        }

        uint64_t deadline = *broadcast_time + inclusion_timeout_;

        if (trace.end_time < deadline) {
            // Trace didn't run long enough to verify inclusion
            continue;
        }

        // Check if transaction was included by deadline
        if (!wasTxIncluded(trace, tx_hash, deadline)) {
            // Violation: transaction not included
            std::ostringstream desc;
            desc << "Transaction " << tx_hash << " broadcast at T=" << *broadcast_time
                 << " was not included in any block by deadline T=" << deadline
                 << " (timeout=" << inclusion_timeout_ << "ms)";

            LivenessViolation v(getName(), desc.str(), deadline, trace.end_time);
            v.details = "Tx: " + tx_hash;
            violations.push_back(v);
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint64_t> DL4Oracle::getTxBroadcastTime(
    const ConsensusTrace& trace,
    const std::string& tx_hash
) const {
    // Look for first TX_RECEIVED event
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::TX_RECEIVED &&
            event.tx_id.has_value() &&
            *event.tx_id == tx_hash) {
            return event.timestamp;
        }
    }

    return std::nullopt;
}

bool DL4Oracle::wasTxIncluded(
    const ConsensusTrace& trace,
    const std::string& tx_hash,
    uint64_t deadline
) const {
    // For Phase 5c simplified implementation:
    // Look for BLOCK_ACCEPTED events that mention this transaction
    // Full implementation would check block contents

    for (const auto& event : trace.events) {
        if (event.timestamp > deadline) {
            break;  // Past deadline
        }

        // Check if any BLOCK_ACCEPTED event mentions this transaction
        // (excludes TX_RECEIVED events which don't indicate inclusion)
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.tx_id.has_value() &&
            *event.tx_id == tx_hash) {
            // Transaction included in a block
            return true;
        }
    }

    return false;
}

std::vector<std::string> DL4Oracle::getAllTransactions(const ConsensusTrace& trace) const {
    std::set<std::string> tx_set;

    // Collect all unique transaction hashes from TX_RECEIVED events
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::TX_RECEIVED &&
            event.tx_id.has_value() &&
            !event.tx_id->empty()) {
            tx_set.insert(*event.tx_id);
        }
    }

    return std::vector<std::string>(tx_set.begin(), tx_set.end());
}

} // namespace test
} // namespace consensus
} // namespace dinero
