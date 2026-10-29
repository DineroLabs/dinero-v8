#pragma once
#include "consensus/orchard_state_transition.h"
#include "consensus/header_chain.h"
#include <optional>

namespace dinero::consensus {
// Value-only, hash-anchored ancestry for the common contextual header gate.
// The host owns the selected branch and keeps this view and Params() stable.
// Implementations must return values from that branch; this interface does not
// establish validity, select a branch, or certify a captured database.
struct OrchardHeaderAncestor {BlockHeader header;uint32_t height;};
class OrchardHeaderAncestry {
public:
    virtual ~OrchardHeaderAncestry() = default;
    virtual std::optional<OrchardHeaderAncestor> GetHeaderValue(const uint256&) const = 0;
    virtual bool GetAsertContextByHash(const uint256&, HeaderAsertContext&,
        std::optional<uint32_t> timing_anchor_height) const = 0;
    virtual bool GetAncestorHashByHash(const uint256&, uint32_t,
        uint256&, uint32_t&) const = 0;
};
enum class OrchardHeaderErrorCode {
    Context, Shape, TimeTooOld, TimeTooNew, Difficulty, ProofOfWork, Checkpoint
};
class OrchardHeaderError : public std::runtime_error {
public:
    explicit OrchardHeaderError(OrchardHeaderErrorCode code)
        : std::runtime_error("Orchard contextual header rejected"), code_(code) {}
    OrchardHeaderErrorCode Code() const noexcept { return code_; }
private:
    OrchardHeaderErrorCode code_;
};
// Missing/evicted ancestry or inconsistent host configuration is a local lookup
// failure, not evidence that a peer's block is consensus-invalid.
class OrchardHeaderLookupError : public std::runtime_error {
public:
    explicit OrchardHeaderLookupError(const char* message) : std::runtime_error(message) {}
};
// Runtime routing context from selected network parameters. Nullopt means
// Orchard is inactive at this height. Invalid local configuration throws a
// lookup error; the transaction or peer never supplies the branch/domain.
[[nodiscard]] std::optional<OrchardBlockContext> SelectedOrchardBlockContext(
    const BlockHeader&, uint32_t height);
// Staged post-activation header gate. The host holds the selected chain/writer
// lock and keeps Params() fixed through application. Every selector read is
// hash-anchored and copies values under the selector's own lock; no raw index
// pointer escapes. The selected parent must already be authenticated by the
// host. Checks configured checkpoints against this parent branch, never the
// best-header height index. Does not select a branch or validate the body.
// TimeTooNew is temporary/retryable: never permanently poison its header.
// now_seconds is the host's current validation clock, not peer-supplied time.
// Ordinary regtest retains its explicit PoW/ASERT bypass; enforce-pow regtest
// and both public networks require exact shared-ASERT bits and actual work.
void CheckOrchardHeaderUnderChainstateLock(
    const BlockHeader&, const BlockHeader& selected_parent,
    const OrchardBlockContext&, const HeaderChainSelector&, uint64_t now_seconds);
// Identical gate for a host-owned immutable replay ancestry. The caller must
// already have independently validated and bound the historical parent.
void CheckOrchardHeaderUnderChainstateLock(
    const BlockHeader&, const BlockHeader& selected_parent,
    const OrchardBlockContext&, const OrchardHeaderAncestry&, uint64_t now_seconds);
// The same selected ancestry/time/difficulty rules for an unmined template.
// This deliberately does not establish proof of work or incoming-block validity.
void CheckOrchardMiningHeaderUnderChainstateLock(
    const BlockHeader&, const BlockHeader&, const OrchardBlockContext&,
    const HeaderChainSelector&, uint64_t now_seconds);
} // namespace dinero::consensus
