#pragma once
#include "consensus/consensus_utxo_set.h"
#include <optional>
#include <span>

namespace dinero::consensus {
struct UTXOPublicationChange {
    OutPoint outpoint;
    std::optional<UTXOEntry> before;
    std::optional<UTXOEntry> after;
};

// Memory half of a caller-owned durable chainstate commit. The activation/writer
// lock must exclude ALL coin/tip writers throughout prepare, CheckReady, database
// commit and publication. Forest readers retain the existing shared-lock API.
// This validates local state agreement, NOT block validity or a durable commit.
class PreparedUTXOPublication {
public:
    [[nodiscard]] static PreparedUTXOPublication PrepareUnderLock(
        ConsensusUTXOSet&, uint32_t from_height, const uint256& from_hash,
        const uint256& from_root, std::span<const UTXOPublicationChange>,
        UtreexoForest next_forest, uint32_t to_height, const uint256& to_hash,
        const uint256& to_root);
    PreparedUTXOPublication(const PreparedUTXOPublication&) = delete;
    PreparedUTXOPublication& operator=(const PreparedUTXOPublication&) = delete;
    PreparedUTXOPublication(PreparedUTXOPublication&&) noexcept;
    PreparedUTXOPublication& operator=(PreparedUTXOPublication&&) = delete;

    // Recheck before the durable write. May allocate while hashing the forest.
    void CheckReadyUnderLock() const;
    // Call only after the exact corresponding outer database commit succeeds.
    // No allocation on the successful path: all insertion nodes and destination
    // capacity were prepared earlier. A broken lock/ownership contract is fatal,
    // allowing restart from the authoritative database instead of continuing
    // with partially published memory. A second/moved-from publication is fatal.
    void PublishAfterCommitUnderLock() && noexcept;
private:
    PreparedUTXOPublication(ConsensusUTXOSet&, uint32_t, uint256, uint256,
        std::span<const UTXOPublicationChange>, UtreexoForest, uint32_t, uint256);
    ConsensusUTXOSet* owner_;
    uint32_t from_height_, to_height_;
    uint256 from_hash_, from_root_, to_hash_;
    std::vector<UTXOPublicationChange> changes_;
    std::unordered_map<OutPoint, UTXOEntry> insertions_;
    UtreexoForest forest_;
    size_t source_size_ = 0, source_buckets_ = 0;
    float source_load_factor_ = 0;
};
} // namespace dinero::consensus
