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
    BlockBody, AuthorizationCoverage, RetiredLegacyPool
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
    explicit OrchardStateLookupError(Status status)
        : std::runtime_error("Orchard state lookup failed"), status_(status) {}
    Status SourceStatus() const noexcept { return status_; }
private:
    Status status_;
};
// Lookups refer ONLY to the selected parent's authenticated chain, under the
// same held writer lock as coin resolution and eventual batch application.
// Missing membership is a successful false, never a swallowed database error.
struct OrchardStateLookups {
    std::function<StatusOr<bool>(const uint256&)> active_anchor;
    std::function<StatusOr<bool>(const uint256&)> spent_nullifier;
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
