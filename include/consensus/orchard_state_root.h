#pragma once
#include "orchard_backend.h"
#include "storage/legacy_retirement.h"
#include "storage/orchard_state.h"
#include <vector>

namespace dinero::consensus {
// No current block hash: a root placed in that block's coinbase must not depend
// on its own resulting header hash. The parent binds the selected ancestry.
struct OrchardStateRootContext {
    orchard::SigningDomain domain;
    uint32_t activation_height = UINT32_MAX;
    uint32_t height = 0;
    uint256 parent_hash;
};
// Draft composite root. Checks profile/shape and authenticates the canonical
// frontier against its root/size, but does NOT prove the legacy amount/SHR1,
// selected ancestry, pool flows or supplied set digests. Runtime callers must
// derive these from their held authenticated chainstate; never RPC inputs.
// Set commitments come from ChainDB's exact-view read/projection APIs.
[[nodiscard]] std::vector<uint8_t> BuildOrchardStateRootPreimage(
    const OrchardStateRootContext&, const storage::LegacyRetirementRecord&,
    const storage::OrchardStoredState&, const storage::OrchardCommitmentSets&);
[[nodiscard]] uint256 ComputeOrchardStateRoot(
    const OrchardStateRootContext&, const storage::LegacyRetirementRecord&,
    const storage::OrchardStoredState&, const storage::OrchardCommitmentSets&);
} // namespace dinero::consensus
