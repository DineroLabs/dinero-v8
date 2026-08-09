#pragma once

#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "primitives/transaction.h"

#include <string>
#include <vector>

namespace dinero::assumeutxo {

struct SnapshotCandidate {
    std::string path;
    uint32_t height = 0;
    uint256 block_hash;
};

enum class SnapshotSelectionStatus {
    Selected,
    NoCandidates,
    NoMatchingActiveLifecycle,
};

struct SnapshotSelection {
    SnapshotSelectionStatus status = SnapshotSelectionStatus::NoCandidates;
    SnapshotCandidate candidate;
};

// Fresh nodes use the first valid candidate (packaging orders newest first).
// A node with persisted AssumeUTXO state MUST instead use the candidate whose
// header matches that lifecycle exactly. Falling forward to a newer snapshot
// would strand or overwrite the older assumed state during an upgrade.
inline SnapshotSelection SelectSnapshotCandidate(
    const std::vector<SnapshotCandidate>& candidates,
    bool lifecycle_active,
    uint32_t lifecycle_height,
    const uint256& lifecycle_block) {
    if (candidates.empty()) {
        return {};
    }
    if (!lifecycle_active) {
        return {SnapshotSelectionStatus::Selected, candidates.front()};
    }
    for (const auto& candidate : candidates) {
        if (candidate.height == lifecycle_height &&
            candidate.block_hash == lifecycle_block) {
            return {SnapshotSelectionStatus::Selected, candidate};
        }
    }
    return {SnapshotSelectionStatus::NoMatchingActiveLifecycle, {}};
}

// A fresh node initializes the canonical genesis coin before deferred snapshot
// loading. Identify that state by exact UTXO identity and contents rather than
// ChainDB tip height: header synchronization may advance persisted metadata
// before the snapshot loader runs, but it must never authorize clearing real
// chainstate.
inline bool IsGenesisOnlyUtxoSet(
    const consensus::IConsensusUTXOSet& utxo_set,
    const Transaction& genesis_coinbase) {
    if (!genesis_coinbase.IsCoinbase() ||
        utxo_set.GetSetSize() != genesis_coinbase.vout.size()) {
        return false;
    }

    const TxId txid = genesis_coinbase.GetTxid();
    for (uint32_t vout = 0; vout < genesis_coinbase.vout.size(); ++vout) {
        const auto* coin = utxo_set.GetCoin(OutPoint(txid, vout));
        const auto& expected = genesis_coinbase.vout[vout];
        if (!coin || coin->height != 0 || !coin->isCoinbase ||
            coin->value != expected.value ||
            coin->scriptPubKey != expected.scriptPubKey ||
            coin->is_confidential != expected.is_confidential ||
            coin->commitment != expected.commitment) {
            return false;
        }
    }
    return true;
}

}  // namespace dinero::assumeutxo
