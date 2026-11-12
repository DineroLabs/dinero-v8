#pragma once
#include "primitives/block.h"
#include "wallet/recovery_tail_types.h"
#include "wallet/snapshot_wallet_types.h"
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dinero {
class WalletManager;
// Owned, independently replayed historical bodies. Built only through the
// Builder below, which enforces the structural facts every consumer relies on
// (hash linkage, tip and genesis binding, snapshot-mode shape). It is an as-of
// source, not a wallet baseline receipt, complete key discovery, or proof that
// the selected chain cannot change later: deeper consensus validation of the
// bodies remains the producer's burden and is NOT certified by this value.
class SelectedWalletHistory final {
public:
    class Builder final {
    public:
        Builder(std::string network, uint256 genesis, uint256 tip)
            : value_(new SelectedWalletHistory()) {
            value_->network_ = std::move(network);
            value_->genesis_ = genesis;
            value_->tip_ = tip;
        }
        // Blocks are appended in ascending height order, first through tip.
        void AppendBlock(Block block) { value_->blocks_.push_back(std::move(block)); }
        // Snapshot mode: bodies start at first_height (> 0) above the
        // snapshot base instead of at genesis.
        void SetSnapshot(uint32_t first_height, SnapshotWalletSourceData snapshot,
                         std::set<std::vector<uint8_t>> scripts) {
            value_->first_height_ = first_height;
            value_->snapshot_ = std::move(snapshot);
            value_->snapshot_scripts_ = std::move(scripts);
        }
        void AddVerifiedLegacyOrigin(const uint256& txid, uint32_t output) {
            value_->verified_legacy_origins_.emplace(txid, output);
        }
        // Typed per-block transparent deltas standing in for owned bodies
        // (v6-recovery stage C): snapshot mode only, instead of AppendBlock,
        // covering exactly first_height through tip.
        void SetTypedTail(std::vector<RecoveryTailBlock> tail) {
            value_->typed_tail_ = std::move(tail);
        }
        // Validates structure; throws std::invalid_argument on violation.
        std::unique_ptr<const SelectedWalletHistory> Build() {
            auto& v = *value_;
            const bool snapshot = v.snapshot_.has_value();
            if (snapshot != (v.first_height_ > 0))
                throw std::invalid_argument("selected-history-mode-mismatch");
            if (!snapshot && v.blocks_.empty())
                throw std::invalid_argument("selected-history-empty");
            for (size_t i = 1; i < v.blocks_.size(); ++i)
                if (v.blocks_[i].header.prev_block_hash != v.blocks_[i - 1].GetHash())
                    throw std::invalid_argument("selected-history-not-linked");
            if (!v.blocks_.empty()) {
                if (v.blocks_.back().GetHash() != v.tip_)
                    throw std::invalid_argument("selected-history-tip-mismatch");
                if (!snapshot && v.blocks_.front().GetHash() != v.genesis_)
                    throw std::invalid_argument("selected-history-genesis-mismatch");
            }
            if (!v.typed_tail_.empty()) {
                if (!snapshot || !v.blocks_.empty())
                    throw std::invalid_argument("selected-history-typed-tail-mode");
                if (v.typed_tail_.front().height != v.first_height_ ||
                    v.typed_tail_.front().parent != v.snapshot_->base)
                    throw std::invalid_argument("selected-history-typed-tail-base");
                for (size_t i = 1; i < v.typed_tail_.size(); ++i)
                    if (v.typed_tail_[i].parent != v.typed_tail_[i - 1].hash ||
                        v.typed_tail_[i].height != v.typed_tail_[i - 1].height + 1)
                        throw std::invalid_argument("selected-history-typed-tail-not-linked");
                if (v.typed_tail_.back().hash != v.tip_)
                    throw std::invalid_argument("selected-history-typed-tail-tip-mismatch");
            }
            return std::unique_ptr<const SelectedWalletHistory>(value_.release());
        }

    private:
        std::unique_ptr<SelectedWalletHistory> value_;
    };

private:
    friend class WalletManager;
    SelectedWalletHistory() = default;
    std::vector<Block> blocks_; // exact first_height_ through captured tip
    // Typed deltas covering the same range when owned raw bodies do not exist
    // (v6-recovery stage C); mutually exclusive with blocks_.
    std::vector<RecoveryTailBlock> typed_tail_;
    uint32_t first_height_=0;
    std::set<std::vector<uint8_t>> snapshot_scripts_;
    std::optional<SnapshotWalletSourceData> snapshot_;
    std::set<std::pair<uint256,uint32_t>> verified_legacy_origins_;
    uint256 tip_;
    std::string network_;
    uint256 genesis_;
};
} // namespace dinero
