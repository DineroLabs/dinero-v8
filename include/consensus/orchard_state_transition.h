#pragma once
#include "consensus/orchard_authorization.h"
#include "storage/orchard_state.h"
#include <span>

namespace dinero::consensus {
struct OrchardBlockContext {
    uint32_t height = 0;
    uint256 block_hash;
    uint256 parent_hash;
    uint32_t activation_height = UINT32_MAX;
    orchard::SigningDomain domain;
};
enum class OrchardStateErrorCode {
    Inactive, Context, ParentState, Anchor, DuplicateNullifier,
    SpentNullifier, DuplicateTransaction, DuplicateInput, PoolBalance, ResourceLimit,
    BlockBody, AuthorizationCoverage, RetiredLegacyPool, StateCommitment
};
class OrchardStateError : public std::runtime_error {
public:
    explicit OrchardStateError(OrchardStateErrorCode code)
        : std::runtime_error("Orchard state transition rejected"), code_(code) {}
    OrchardStateErrorCode Code() const noexcept { return code_; }
private:
    OrchardStateErrorCode code_;
};
class OrchardStateLookupError : public std::runtime_error {
public:
    // Optional constant operation label for diagnostics. Classification and
    // original storage status are preserved; no missing read becomes success.
    explicit OrchardStateLookupError(Status status, const char* operation = nullptr)
        : std::runtime_error(operation ? std::string("Orchard state lookup failed at ") +
              operation + " (" + StatusToString(status) + ")" : "Orchard state lookup failed"),
          status_(status), operation_(operation) {}
    Status SourceStatus() const noexcept { return status_; }
    const char* Operation() const noexcept { return operation_; }
private:
    Status status_;
    const char* operation_;
};
// Lookups refer ONLY to the selected parent's authenticated chain, under the
// same held writer lock as coin resolution and eventual batch application.
// Missing membership is a successful false, never a swallowed database error.
struct OrchardStateLookups {
    std::function<StatusOr<bool>(const uint256&)> active_anchor;
    std::function<StatusOr<bool>(const uint256&)> spent_nullifier;
};
// Selected-parent transaction context. Unlike OrchardBlockContext this cannot
// carry a candidate block identity. Validation does not publish chain state and
// is not an admission, reservation, or lasting selected-parent certificate.
struct OrchardTransactionContext {
    uint32_t height = 0;
    uint256 parent_hash;
    uint32_t activation_height = UINT32_MAX;
    orchard::SigningDomain domain;
};
class CheckedOrchardTransactions;
[[nodiscard]] CheckedOrchardTransactions CheckOrchardTransactions(
    const OrchardTransactionContext&, const std::optional<storage::OrchardStoredState>&,
    std::span<const VerifiedOrchardAuthorizations>, const OrchardStateLookups&);

// Immutable result for one ordered sequence against one selected parent. The
// caller owns chain/coin/pool locks and must supply every relevant transaction
// in order. Pending conflicts, policy, persistence and relay remain separate.
class CheckedOrchardTransactions {
public:
    const auto& Parent() const noexcept { return parent_; }
    uint64_t PoolBalance() const noexcept { return pool_balance_; }
    uint64_t TreeSize() const noexcept { return tree_size_; }
    const uint256& Anchor() const noexcept { return anchor_; }
    const std::string& Frontier() const noexcept { return frontier_; }
    const auto& Nullifiers() const noexcept { return nullifiers_; }
    const auto& Flows() const noexcept { return flows_; }
    uint64_t Fees() const noexcept { return fees_; }
private:
    friend CheckedOrchardTransactions CheckOrchardTransactions(
        const OrchardTransactionContext&, const std::optional<storage::OrchardStoredState>&,
        std::span<const VerifiedOrchardAuthorizations>, const OrchardStateLookups&);
    CheckedOrchardTransactions(std::optional<storage::OrchardStoredState> parent,
        uint64_t balance, uint64_t size, uint256 anchor, std::string frontier,
        std::vector<uint256> nullifiers, std::vector<OrchardValueFlow> flows, uint64_t fees)
        : parent_(std::move(parent)), pool_balance_(balance), tree_size_(size), anchor_(anchor),
          frontier_(std::move(frontier)), nullifiers_(std::move(nullifiers)),
          flows_(std::move(flows)), fees_(fees) {}
    const std::optional<storage::OrchardStoredState> parent_;
    const uint64_t pool_balance_, tree_size_;
    const uint256 anchor_;
    const std::string frontier_;
    const std::vector<uint256> nullifiers_;
    const std::vector<OrchardValueFlow> flows_;
    const uint64_t fees_;
};
class PreparedOrchardState;
[[nodiscard]] PreparedOrchardState PrepareOrchardStateTransition(
    const OrchardBlockContext&, const std::optional<storage::OrchardStoredState>&,
    std::span<const VerifiedOrchardAuthorizations>, const OrchardStateLookups&);

// Sealed Orchard-state update, NOT a full-block validity or freshness token.
// The caller must include every Orchard transaction in canonical block order,
// validate mixed transparent spends/outputs, bound coinbase using these fees,
// and hold the selected branch/coin view lock through atomic application.
class PreparedOrchardState {
public:
    const auto& Parent() const noexcept { return parent_; }
    const auto& Next() const noexcept { return next_; }
    const auto& Nullifiers() const noexcept { return nullifiers_; }
    const auto& Flows() const noexcept { return flows_; }
    uint64_t Fees() const noexcept { return fees_; }
private:
    friend PreparedOrchardState PrepareOrchardStateTransition(
        const OrchardBlockContext&, const std::optional<storage::OrchardStoredState>&,
        std::span<const VerifiedOrchardAuthorizations>, const OrchardStateLookups&);
    PreparedOrchardState(std::optional<storage::OrchardStoredState> parent,
        storage::OrchardStoredState next, std::vector<uint256> nullifiers,
        std::vector<OrchardValueFlow> flows, uint64_t fees)
        : parent_(std::move(parent)), next_(std::move(next)), nullifiers_(std::move(nullifiers)),
          flows_(std::move(flows)), fees_(fees) {}
    const std::optional<storage::OrchardStoredState> parent_;
    const storage::OrchardStoredState next_;
    const std::vector<uint256> nullifiers_;
    const std::vector<OrchardValueFlow> flows_;
    const uint64_t fees_;
};
} // namespace dinero::consensus
