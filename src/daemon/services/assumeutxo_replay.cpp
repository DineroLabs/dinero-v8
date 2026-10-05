#include "daemon/services/assumeutxo_replay.h"

#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace dinero::assumeutxo {

AssumeUtxoReplayEngine::AssumeUtxoReplayEngine()
    : network_(Params().network_id),
      genesis_hash_(uint256::FromHexUnsafe(Params().genesis_hash)),
      set_(consensus::ConsensusUTXOSet::CreateForReplay()),
      validator_(std::make_unique<consensus::BlockValidator>(set_.get(),
          [this](const uint256& parent, uint32_t wanted) -> std::optional<uint64_t> {
              if (!seeded_ || parent != tip_hash_ || wanted > last_height_) return std::nullopt;
              try { return headers_.LockMedianTimePast(parent, wanted); }
              catch (...) { unavailable_ = true; throw; }
          })),
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

void AssumeUtxoReplayEngine::CheckAvailable() const {
    if (unavailable_) throw std::runtime_error("Replay engine has unavailable header storage");
}

bool AssumeUtxoReplayEngine::SeedGenesis(const Block& genesis_block, std::string& error) {
    if (unavailable_ || seeded_ || Params().network_id != network_ ||
        uint256::FromHexUnsafe(Params().genesis_hash) != genesis_hash_) {
        error = "replay genesis already seeded or network changed";
        return false;
    }
    Transaction expected;
    bool mutated = false;
    if (genesis_block.GetHash() != genesis_hash_ ||
        !genesis_block.header.prev_block_hash.IsNull() ||
        genesis_block.vtx.size() != 1 ||
        !TransactionSerializer::Deserialize(expected, Params().genesis.genesisCoinbaseHex) ||
        genesis_block.vtx.front().Serialize(TxSerializationMode::WithWitness) !=
            expected.Serialize(TxSerializationMode::WithWitness) ||
        consensus::ComputeMerkleRoot(genesis_block.vtx, &mutated) != genesis_block.header.merkle_root ||
        mutated || !headers_.Validate(genesis_block.header)) {
        error = "replay genesis identity or body mismatch";
        return false;
    }
    // Mirror genesis_init.cpp's ChainDB seeding: every output of the genesis
    // coinbase becomes a coin at height 0 with coinbase=true, INCLUDING
    // OP_RETURN outputs (ConnectBlock's ProcessTransaction skips those, which
    // is exactly why genesis cannot go through ConnectAndAdvance). No utreexo
    // leaves are added — the live forest excludes genesis too (the height-0
    // checkpoint is an empty forest).
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
    try { headers_.AppendValidated(genesis_block.header, 0); }
    catch (...) { unavailable_ = true; error = "replay header storage unavailable"; return false; }
    tip_hash_ = genesis_hash_;
    seeded_ = true;
    return true;
}

bool AssumeUtxoReplayEngine::ConnectAndAdvance(const Block& block, uint32_t height,
                                               const uint256& block_hash,
                                               std::string& error) {
    if (unavailable_ || !seeded_ || Params().network_id != network_ ||
        uint256::FromHexUnsafe(Params().genesis_hash) != genesis_hash_) {
        error = "replay requires seeded selected genesis";
        return false;
    }
    if (last_height_ == std::numeric_limits<uint32_t>::max() || height != last_height_ + 1) {
        error = "replay heights must be strictly ascending (got " +
                std::to_string(height) + " after " + std::to_string(last_height_) + ")";
        return false;
    }
    bool mutated = false;
    if (block_hash != block.GetHash() || block.header.prev_block_hash != tip_hash_ ||
        block.vtx.empty() || consensus::ComputeMerkleRoot(block.vtx, &mutated) != block.header.merkle_root ||
        mutated) {
        error = "replay block identity, parent or Merkle mismatch";
        return false;
    }
    try {
        if (!headers_.Validate(block.header)) {
            error = "replay header validation failed";
            return false;
        }
    } catch (...) {
        unavailable_ = true; error = "replay header ancestry unavailable"; return false;
    }
    consensus::BlockUndo undo;
    try {
        if (!validator_->ConnectBlock(block, height, block_hash, undo, error)) return false;
    } catch (...) {
        unavailable_ = true; error = "replay body or ancestry unavailable"; return false;
    }
    // No replay-spool mutex or SQL transaction spans expensive body validation.
    // If this private append fails after coin effects, retire the whole engine;
    // no partial state may be used as a validated prefix or promotion input.
    try { headers_.AppendValidated(block.header, height); }
    catch (...) { unavailable_ = true; error = "replay header storage unavailable"; return false; }
    // Capture undo for the audited tail window (ring semantics: drop oldest
    // when the deque exceeds the window). BlockUndo is movable — utreexo_delta,
    // spent_coins, and optional fields all move without extra allocation.
    try {
    if (undo_tail_window_ > 0) {
        // CRITICAL: strip the pre-block UTXO-set snapshot ConnectBlock attaches
        // when the backend supports snapshot/restore — it deep-copies the whole
        // set + serialized forest (~30MB at mainnet scale); retaining 1024 of
        // them would OOM the final pass. The flatfile undo format never
        // serializes it (BlockUndo::Serialize), and disconnect durability uses
        // spent_coins + frontier + the UD sidecar, never this snapshot.
        undo.pre_block_snapshot.reset();
        undo_tail_.push_back(CapturedUndo{height, block_hash, std::move(undo)});
        while (undo_tail_.size() > undo_tail_window_) undo_tail_.pop_front();
    }
    } catch (...) {
        unavailable_ = true; error = "replay undo tail unavailable"; return false;
    }
    last_height_ = height;
    tip_hash_ = block_hash;
    return true;
}

std::optional<consensus::UTXOEntry>
AssumeUtxoReplayEngine::CopyPrefixCoin(const OutPoint& point) const {
    CheckAvailable();
    if (!seeded_) throw std::logic_error("Replay prefix has no genesis");
    const auto* coin = set_->GetCoin(point);
    if (!coin) return std::nullopt;
    return *coin;
}

void AssumeUtxoReplayEngine::SetUndoTailWindow(uint32_t window) {
    CheckAvailable();
    undo_tail_window_ = window;
    undo_tail_.clear();
}

const std::unordered_map<OutPoint, consensus::UTXOEntry>&
AssumeUtxoReplayEngine::ProvenUtxos() const {
    CheckAvailable();
    return set_->GetUTXOs();
}

const consensus::UtreexoForest* AssumeUtxoReplayEngine::Forest() const {
    CheckAvailable();
    return &set_->GetForest();
}

const consensus::shielded::CommitmentTree* AssumeUtxoReplayEngine::ShieldedTree() const {
    CheckAvailable();
    return shielded_tree_.get();
}

const consensus::shielded::NullifierSet* AssumeUtxoReplayEngine::ShieldedNullifiers() const {
    CheckAvailable();
    return shielded_nullifiers_.get();
}

const consensus::shielded::AnchorHistory* AssumeUtxoReplayEngine::ShieldedAnchors() const {
    CheckAvailable();
    return shielded_anchor_history_.get();
}

uint64_t AssumeUtxoReplayEngine::UtxoCount() const { CheckAvailable(); return set_->GetUTXOs().size(); }

std::string AssumeUtxoReplayEngine::RecordsDigestHex() const {
    CheckAvailable();
    return consensus::ComputeUtxoRecordsDigest(set_->GetUTXOs()).GetHex();
}

std::string AssumeUtxoReplayEngine::UtreexoRootHex() const {
    CheckAvailable();
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
