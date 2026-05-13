#include "economic_trace.h"
#include <algorithm>
#include <functional>
#include <set>

namespace dinero {
namespace economic {
namespace test {

// Simple hash combiner (FNV-1a inspired)
static uint64_t hashCombine(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

static uint64_t hashString(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

uint64_t EconomicTrace::computeHash() const {
    uint64_t hash = rng_seed;

    // Hash scenario name
    hash = hashCombine(hash, hashString(scenario_name));

    // Hash nodes
    for (const auto& node : nodes) {
        hash = hashCombine(hash, hashString(node));
    }

    // Hash policy configuration
    hash = hashCombine(hash, policy.min_relay_fee_una);
    hash = hashCombine(hash, policy.dust_threshold_una);
    hash = hashCombine(hash, policy.max_mempool_size_bytes);

    // Hash all actions
    for (const auto& action : actions) {
        hash = hashCombine(hash, static_cast<uint64_t>(action.type));
        hash = hashCombine(hash, action.timestamp);
        hash = hashCombine(hash, action.sequence_number);

        if (action.node_id) {
            hash = hashCombine(hash, hashString(*action.node_id));
        }
        if (action.tx_id) {
            hash = hashCombine(hash, hashString(*action.tx_id));
        }
        if (action.fee_una) {
            hash = hashCombine(hash, *action.fee_una);
        }
        if (action.fee_rate) {
            hash = hashCombine(hash, static_cast<uint64_t>(*action.fee_rate * 1000));
        }
    }

    // Hash all events
    for (const auto& event : events) {
        hash = hashCombine(hash, static_cast<uint64_t>(event.type));
        hash = hashCombine(hash, event.timestamp);
        hash = hashCombine(hash, event.sequence_number);
        hash = hashCombine(hash, hashString(event.node_id));
        hash = hashCombine(hash, event.success ? 1 : 0);

        if (event.tx_id) {
            hash = hashCombine(hash, hashString(*event.tx_id));
        }
        if (event.fee_una) {
            hash = hashCombine(hash, *event.fee_una);
        }
        if (event.block_height) {
            hash = hashCombine(hash, *event.block_height);
        }
    }

    // Hash all state snapshots (focus on economic state)
    for (const auto& snapshot : snapshots) {
        hash = hashCombine(hash, hashString(snapshot.node_id));
        hash = hashCombine(hash, snapshot.timestamp);
        hash = hashCombine(hash, snapshot.mempool_tx_count);
        hash = hashCombine(hash, snapshot.mempool_total_fees);
        hash = hashCombine(hash, snapshot.chain_height);
        hash = hashCombine(hash, snapshot.min_relay_fee_una);
        hash = hashCombine(hash, snapshot.dust_threshold_una);
    }

    return hash;
}

std::vector<EconomicEvent> EconomicTrace::getEventsForNode(const NodeID& node_id) const {
    std::vector<EconomicEvent> node_events;
    for (const auto& event : events) {
        if (event.node_id == node_id) {
            node_events.push_back(event);
        }
    }
    return node_events;
}

std::vector<EconomicState> EconomicTrace::getSnapshotsForNode(const NodeID& node_id) const {
    std::vector<EconomicState> node_snapshots;
    for (const auto& snapshot : snapshots) {
        if (snapshot.node_id == node_id) {
            node_snapshots.push_back(snapshot);
        }
    }
    return node_snapshots;
}

std::optional<EconomicState> EconomicTrace::getStateAt(const NodeID& node_id, uint64_t timestamp) const {
    std::optional<EconomicState> result;

    // Find the latest snapshot before or at the given timestamp
    for (const auto& snapshot : snapshots) {
        if (snapshot.node_id == node_id && snapshot.timestamp <= timestamp) {
            if (!result || snapshot.timestamp > result->timestamp) {
                result = snapshot;
            }
        }
    }

    return result;
}

std::vector<TxID> EconomicTrace::getAllSubmittedTxs() const {
    std::set<TxID> unique_txs;

    for (const auto& action : actions) {
        if (action.tx_id) {
            unique_txs.insert(*action.tx_id);
        }
    }

    for (const auto& event : events) {
        if (event.tx_id) {
            unique_txs.insert(*event.tx_id);
        }
    }

    return std::vector<TxID>(unique_txs.begin(), unique_txs.end());
}

std::vector<TxID> EconomicTrace::getAllConfirmedTxs() const {
    std::set<TxID> confirmed_txs;

    for (const auto& event : events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK && event.tx_id) {
            confirmed_txs.insert(*event.tx_id);
        }
    }

    return std::vector<TxID>(confirmed_txs.begin(), confirmed_txs.end());
}

std::vector<TxID> EconomicTrace::getFinalMempoolTxs() const {
    if (snapshots.empty()) {
        return {};
    }

    // Get the most recent snapshot
    const EconomicState* latest = nullptr;
    for (const auto& snapshot : snapshots) {
        if (!latest || snapshot.timestamp > latest->timestamp) {
            latest = &snapshot;
        }
    }

    if (!latest) {
        return {};
    }

    std::vector<TxID> mempool_txs;
    for (const auto& entry : latest->mempool_entries) {
        mempool_txs.push_back(entry.tx_id);
    }

    return mempool_txs;
}

std::vector<EconomicEvent> EconomicTrace::getEventsByType(EconomicEventType type) const {
    std::vector<EconomicEvent> filtered_events;
    for (const auto& event : events) {
        if (event.type == type) {
            filtered_events.push_back(event);
        }
    }
    return filtered_events;
}

std::vector<EconomicEvent> EconomicTrace::getEventsForTx(const TxID& tx_id) const {
    std::vector<EconomicEvent> tx_events;
    for (const auto& event : events) {
        if (event.tx_id && *event.tx_id == tx_id) {
            tx_events.push_back(event);
        }
    }
    return tx_events;
}

std::optional<uint64_t> EconomicTrace::getTxFee(const TxID& tx_id) const {
    // Look for TX_INCLUDED_IN_BLOCK event with fee information
    for (const auto& event : events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id &&
            event.fee_una) {
            return event.fee_una;
        }
    }

    // Alternatively, check TX_ACCEPTED_TO_MEMPOOL event
    for (const auto& event : events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.tx_id && *event.tx_id == tx_id &&
            event.fee_una) {
            return event.fee_una;
        }
    }

    return std::nullopt;
}

bool EconomicTrace::isTxConfirmed(const TxID& tx_id) const {
    for (const auto& event : events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.tx_id && *event.tx_id == tx_id) {
            return true;
        }
    }
    return false;
}

bool EconomicTrace::isTxInMempool(const TxID& tx_id) const {
    auto final_mempool = getFinalMempoolTxs();
    return std::find(final_mempool.begin(), final_mempool.end(), tx_id) != final_mempool.end();
}

double EconomicTrace::getAverageFeeRate() const {
    uint64_t total_fees = 0;
    uint64_t total_size = 0;

    for (const auto& event : events) {
        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK &&
            event.fee_una && event.tx_size_bytes) {
            total_fees += *event.fee_una;
            total_size += *event.tx_size_bytes;
        }
    }

    if (total_size == 0) {
        return 0.0;
    }

    return static_cast<double>(total_fees) / static_cast<double>(total_size);
}

uint64_t EconomicTrace::getFinalMempoolFees() const {
    if (snapshots.empty()) {
        return 0;
    }

    // Get the most recent snapshot
    const EconomicState* latest = nullptr;
    for (const auto& snapshot : snapshots) {
        if (!latest || snapshot.timestamp > latest->timestamp) {
            latest = &snapshot;
        }
    }

    return latest ? latest->mempool_total_fees : 0;
}

} // namespace test
} // namespace economic
} // namespace dinero
