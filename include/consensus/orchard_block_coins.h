#pragma once
#include "consensus/orchard_state_transition.h"
#include "primitives/orchard_block_reader.h"

namespace dinero::consensus {
enum class OrchardBlockCoinErrorCode {
    Context, Body, RetiredPool, DuplicateTransaction, MissingCoin, DuplicateInput,
    OutputCollision, Amount, ImmatureCoinbase, Locks, Script, Reward
};
class OrchardBlockCoinError : public std::runtime_error {
public:
    explicit OrchardBlockCoinError(OrchardBlockCoinErrorCode code)
        : std::runtime_error("Orchard mixed block coin validation rejected"), code_(code) {}
    OrchardBlockCoinErrorCode Code() const noexcept { return code_; }
private:
    OrchardBlockCoinErrorCode code_;
};
struct OrchardTransactionCoins {
    TxId txid;
    bool coinbase = false;
    std::vector<std::pair<OutPoint, UTXOEntry>> spent;
    std::vector<std::pair<OutPoint, UTXOEntry>> created;
    uint64_t fee = 0;
};
struct OrchardCoinChange {
    OutPoint outpoint;
    std::optional<UTXOEntry> before;
    std::optional<UTXOEntry> after;
};
class PreparedOrchardBlockCoins;
[[nodiscard]] PreparedOrchardBlockCoins PrepareOrchardBlockCoinsUnderChainstateLock(
    const OrchardBlockCandidate&, const OrchardBlockContext&, const ChainStateView&,
    const OrchardBranchMtpLookup&, bool require_witness_commitment);

// Validated authorization, ordered coin spending and fees, NOT full block
// validity: PoW/header context, Utreexo proofs, block resource limits, Orchard
// anchors/nullifiers/pool and atomic application must also succeed. Selected
// Params() and the authenticated parent view must remain fixed under the host's
// chainstate lock until application. No external view is mutated here.
class PreparedOrchardBlockCoins {
public:
    const uint256& BlockHash() const noexcept { return block_hash_; }
    const auto& Transactions() const noexcept { return transactions_; }
    const auto& Changes() const noexcept { return changes_; }
    const auto& Authorizations() const noexcept { return authorizations_; }
    uint64_t TotalFees() const noexcept { return fees_; }
private:
    friend PreparedOrchardBlockCoins PrepareOrchardBlockCoinsUnderChainstateLock(
        const OrchardBlockCandidate&, const OrchardBlockContext&, const ChainStateView&,
        const OrchardBranchMtpLookup&, bool);
    PreparedOrchardBlockCoins(uint256 hash, std::vector<OrchardTransactionCoins> transactions,
        std::vector<OrchardCoinChange> changes,
        std::vector<VerifiedOrchardAuthorizations> authorizations, uint64_t fees)
        : block_hash_(hash), transactions_(std::move(transactions)), changes_(std::move(changes)),
          authorizations_(std::move(authorizations)), fees_(fees) {}
    const uint256 block_hash_;
    const std::vector<OrchardTransactionCoins> transactions_;
    const std::vector<OrchardCoinChange> changes_;
    const std::vector<VerifiedOrchardAuthorizations> authorizations_;
    const uint64_t fees_;
};
} // namespace dinero::consensus
