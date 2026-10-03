#pragma once
#include "consensus/orchard_transparent.h"

namespace dinero::consensus {
class VerifiedOrchardAuthorizations;
[[nodiscard]] VerifiedOrchardAuthorizations VerifyOrchardAuthorizations(
    const OrchardCoinSnapshot& snapshot, orchard::SigningDomain domain,
    uint32_t candidate_height, const OrchardBranchMtpLookup& branch_mtp);

// Both cryptographic authorization paths, balance, maturity and locks for one
// owned transaction/context. NOT full admission: anchor membership, nullifier
// freshness, pool state, activation and atomic application remain mandatory.
class VerifiedOrchardAuthorizations {
public:
    const VerifiedOrchardTransparentInputs& Transparent() const noexcept { return transparent_; }
    const orchard::VerifiedEnvelopeAuthorization& Orchard() const noexcept { return orchard_; }
    const orchard::TransactionEnvelope& Transaction() const noexcept {
        return transparent_.Snapshot().Transaction();
    }
private:
    friend VerifiedOrchardAuthorizations VerifyOrchardAuthorizations(
        const OrchardCoinSnapshot&, orchard::SigningDomain, uint32_t,
        const OrchardBranchMtpLookup&);
    VerifiedOrchardAuthorizations(VerifiedOrchardTransparentInputs transparent,
                                 orchard::VerifiedEnvelopeAuthorization orchard)
        : transparent_(std::move(transparent)), orchard_(std::move(orchard)) {}
    const VerifiedOrchardTransparentInputs transparent_;
    const orchard::VerifiedEnvelopeAuthorization orchard_;
};
} // namespace dinero::consensus
