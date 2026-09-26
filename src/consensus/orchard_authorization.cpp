#include "consensus/orchard_authorization.h"

namespace dinero::consensus {
VerifiedOrchardAuthorizations VerifyOrchardAuthorizations(
    const OrchardCoinSnapshot& snapshot, orchard::SigningDomain domain,
    uint32_t candidate_height, const OrchardBranchMtpLookup& branch_mtp) {
    auto transparent = VerifyOrchardTransparentInputs(snapshot, domain, candidate_height, branch_mtp);
    // Verify the Orchard proof/signatures using that exact owned snapshot.
    // No independently supplied digest, cached effects verdict or second view.
    auto orchard = transparent.Snapshot().VerifyOrchardAuthorization(domain);
    const auto& tx = transparent.Snapshot().Transaction();
    if (orchard.Orchard().SigningDigest() != transparent.OrchardIntent() ||
        orchard.Txid() != tx.Txid() || orchard.Wtxid() != tx.Wtxid() ||
        orchard.CanonicalBytes() != tx.CanonicalBytes()) {
        throw std::logic_error("inconsistent Orchard authorization identity");
    }
    return VerifiedOrchardAuthorizations(std::move(transparent), std::move(orchard));
}
} // namespace dinero::consensus
