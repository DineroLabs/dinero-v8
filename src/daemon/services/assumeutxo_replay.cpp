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

bool AssumeUtxoReplayEngine::SeedGenesis(const Block& genesis_block, std::string& error) {
    // Mirror genesis_init.cpp's ChainDB seeding: every output of the genesis
    // coinbase becomes a coin at height 0 with coinbase=true, INCLUDING
    // OP_RETURN outputs (ConnectBlock's ProcessTransaction skips those, which
    // is exactly why genesis cannot go through ConnectAndAdvance). No utreexo
    // leaves are added — the live forest excludes genesis too (the height-0
    // checkpoint is an empty forest).
    if (genesis_block.vtx.empty()) {
        return true;  // no coinbase -> nothing to seed
    }
    const Transaction& genesis_tx = genesis_block.vtx[0];
    const TxId txid = genesis_tx.GetTxid();
    for (uint32_t vout = 0; vout < genesis_tx.vout.size(); ++vout) {
        const TxOutput& output = genesis_tx.vout[vout];
        const OutPoint outpoint(txid, vout);
        const consensus::UTXOEntry entry(
            output.value,
            output.scriptPubKey,
            /*height=*/0,
            /*isCoinbase=*/true,
            output.is_confidential,
            output.commitment);
        if (!set_->AddCoin(outpoint, entry)) {
            error = "SeedGenesis: duplicate genesis coin " +
                    txid.AsUint256().GetHex() + ":" + std::to_string(vout);
            return false;
        }
    }
    return true;
}

bool AssumeUtxoReplayEngine::ConnectAndAdvance(const Block& block, uint32_t height,
                                               const uint256& block_hash,
                                               std::string& error) {
    if (any_connected_ && height != last_height_ + 1) {
        error = "replay heights must be strictly ascending (got " +
                std::to_string(height) + " after " + std::to_string(last_height_) + ")";
        return false;
    }
    consensus::BlockUndo undo;
    if (!validator_->ConnectBlock(block, height, block_hash, undo, error)) {
        return false;
    }
    // Capture undo for the audited tail window (ring semantics: drop oldest
    // when the deque exceeds the window). BlockUndo is movable — utreexo_delta,
    // spent_coins, and optional fields all move without extra allocation.
    if (undo_tail_window_ > 0) {
        undo_tail_.push_back(CapturedUndo{height, block_hash, std::move(undo)});
        while (undo_tail_.size() > undo_tail_window_) undo_tail_.pop_front();
    }
    last_height_ = height;
    any_connected_ = true;
    return true;
}

void AssumeUtxoReplayEngine::SetUndoTailWindow(uint32_t window) {
    undo_tail_window_ = window;
    undo_tail_.clear();
}

const std::unordered_map<OutPoint, consensus::UTXOEntry>&
AssumeUtxoReplayEngine::ProvenUtxos() const {
    return set_->GetUTXOs();
}

const consensus::UtreexoForest* AssumeUtxoReplayEngine::Forest() const {
    return &set_->GetForest();
}

const consensus::shielded::CommitmentTree* AssumeUtxoReplayEngine::ShieldedTree() const {
    return shielded_tree_.get();
}

const consensus::shielded::NullifierSet* AssumeUtxoReplayEngine::ShieldedNullifiers() const {
    return shielded_nullifiers_.get();
}

const consensus::shielded::AnchorHistory* AssumeUtxoReplayEngine::ShieldedAnchors() const {
    return shielded_anchor_history_.get();
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
