#pragma once

#include "consensus/orchard_block_coins.h"
#include "consensus/utreexo_stump.h"
#include <functional>
#include <map>
#include <set>

namespace dinero::consensus {
enum class OrchardCandidateCoinErrorCode {
    Context, Proof, DuplicateTransaction, HistoricalTransaction, DuplicateInput,
    InputOrder, Metadata, OutputCollision
};
class OrchardCandidateCoinError final : public std::runtime_error {
public:
    explicit OrchardCandidateCoinError(OrchardCandidateCoinErrorCode code)
        : std::runtime_error("Orchard candidate coin capture rejected"), code_(code) {}
    OrchardCandidateCoinErrorCode Code() const noexcept { return code_; }
private:
    OrchardCandidateCoinErrorCode code_;
};

// Immutable, candidate-only input view. The caller must supply coin metadata
// and complete transaction membership from ONE independently authenticated
// parent, under its ownership for the whole capture. In particular, legacy
// Utreexo leaves do not authenticate creation height or coinbase status: the
// peer's spent-output metadata must never be the parent authority here.
//
// The stump authenticates exact external leaves. Complete transaction absence
// proves the candidate's output outpoints absent, and a checked parent-coin
// read must agree. The parent may itself derive absence from its complete
// transaction catalog. No other missing lookup is
// reported as NotFound. Same-block inputs are resolved in transaction order by
// the shared block validator, not inserted into this parent view.
//
// Capture performs no signature/Orchard authorization, state transition or
// persistence. The result does not authenticate its source, acknowledge CSN
// readiness, or replace the canonical writer's selected-parent recheck.
class OrchardCandidateCoinView final : public ChainStateView {
public:
    using TransactionMembership = std::function<StatusOr<bool>(const TxId&)>;
    [[nodiscard]] static OrchardCandidateCoinView Capture(
        const OrchardBlockCandidate&, const OrchardBlockContext&,
        const BlockHeader& parent_header, const UtreexoStump& parent_stump,
        const ChainStateView& authenticated_parent,
        const TransactionMembership& authenticated_transactions);

    StatusOr<UTXOEntry> getCoin(const OutPoint&) const override;
    bool hasCoin(const OutPoint&) const override;
    uint32_t getHeight() const override { return height_; }
    const uint256& ParentHash() const noexcept { return parent_hash_; }
    const uint256& BlockHash() const noexcept { return block_hash_; }
    size_t CapturedInputs() const noexcept { return inputs_.size(); }
    size_t ProvedOutputAbsences() const noexcept { return absent_.size(); }
private:
    OrchardCandidateCoinView(uint32_t height, uint256 parent, uint256 block)
        : height_(height), parent_hash_(parent), block_hash_(block) {}
    uint32_t height_;
    uint256 parent_hash_, block_hash_;
    std::map<OutPoint, UTXOEntry> inputs_;
    std::set<OutPoint> absent_;
};
} // namespace dinero::consensus
