#pragma once
#include "consensus/orchard_state_transition.h"

namespace dinero::consensus {
class HeaderChainSelector;
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
} // namespace dinero::consensus
