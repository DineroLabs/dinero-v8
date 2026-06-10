#include "daemon/services/assumeutxo_replay.h"

#include <cstring>
#include <stdexcept>

namespace dinero::assumeutxo {

AssumeUtxoReplayEngine::AssumeUtxoReplayEngine()
    : set_(std::make_unique<consensus::ConsensusUTXOSet>()),
      validator_(std::make_unique<consensus::BlockValidator>(set_.get())),
      shielded_tree_(std::make_unique<consensus::shielded::CommitmentTree>()),
      shielded_nullifiers_(std::make_unique<consensus::shielded::NullifierSet>()),
      shielded_anchor_history_(std::make_unique<consensus::shielded::AnchorHistory>()) {
    // NullifierSet is sqlite-backed; un-opened, Contains() always returns
    // false (double-spends invisible) and Insert() always returns false
    // (treated as already-present). Open an ephemeral in-memory db — replay
    // state is never persisted.
    if (shielded_nullifiers_->Open(":memory:") !=
        consensus::shielded::NullifierSet::OpenResult::Ok) {
        throw std::runtime_error(
            "AssumeUtxoReplayEngine: failed to open in-memory nullifier set");
    }
    // Same wiring as production ConnectTip (chainstate_service.cpp): without
    // shielded state a stateful validator rejects every shielded tx with
    // "Shielded state unavailable" — a false fatal on honest history.
    validator_->setShieldedState(shielded_tree_.get(), shielded_nullifiers_.get(),
                                 shielded_anchor_history_.get());
    // Pin the mode: replay must never route through the script-skipping
    // STATELESS path even if the BlockValidator default changes.
    validator_->setValidationMode(consensus::ValidationMode::STATEFUL);
}

AssumeUtxoReplayEngine::~AssumeUtxoReplayEngine() = default;

bool AssumeUtxoReplayEngine::ConnectAndAdvance(const Block& block, uint32_t height,
                                               const uint256& block_hash,
                                               std::string& error) {
    if (any_connected_ && height != last_height_ + 1) {
        error = "replay heights must be strictly ascending (got " +
                std::to_string(height) + " after " + std::to_string(last_height_) + ")";
        return false;
    }
    consensus::BlockUndo undo;  // replay does not persist undo
    if (!validator_->ConnectBlock(block, height, block_hash, undo, error)) {
        return false;
    }
    last_height_ = height;
    any_connected_ = true;
    return true;
}

uint64_t AssumeUtxoReplayEngine::UtxoCount() const { return set_->GetUTXOs().size(); }

std::string AssumeUtxoReplayEngine::RecordsDigestHex() const {
    return consensus::ComputeUtxoRecordsDigest(set_->GetUTXOs()).GetHex();
}

std::string AssumeUtxoReplayEngine::UtreexoRootHex() const {
    const consensus::UtreexoHash root = set_->GetForest().getCommitment();
    uint256 h;
    if (root.size() == 32) {
        std::memcpy(h.data, root.data(), 32);
    } else {
        h.SetNull();
    }
    return h.GetHex();
}

}  // namespace dinero::assumeutxo
