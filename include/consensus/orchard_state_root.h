#pragma once
#include "orchard_backend.h"
#include "storage/legacy_retirement.h"
#include "storage/orchard_state.h"
#include "storage/orchard_catalog_state.h"
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

// DNORST02 draft: authenticates the complete canonical PARENT catalog record.
// Committing to the current catalog would cycle through the current coinbase
// txid. The snapshot must additionally derive its base catalog by applying the
// authenticated base body to this parent. This explicit API does not change
// DNORST01 or enable a consensus activation by itself.
[[nodiscard]] std::vector<uint8_t> BuildOrchardCatalogStateRootPreimage(
    const OrchardStateRootContext&, const storage::LegacyRetirementRecord&,
    const storage::OrchardStoredState&, const storage::OrchardCommitmentSets&,
    const storage::catalog::State& parent_catalog);
[[nodiscard]] uint256 ComputeOrchardCatalogStateRoot(
    const OrchardStateRootContext&, const storage::LegacyRetirementRecord&,
    const storage::OrchardStoredState&, const storage::OrchardCommitmentSets&,
    const storage::catalog::State& parent_catalog);
// Explicit height selection, shared by every runtime producer/verifier. Missing
// parent authority at an active DNORST02 height is an error, never a v1 fallback.
[[nodiscard]] uint256 ComputeOrchardSelectedStateRoot(
    const OrchardStateRootContext&, const storage::LegacyRetirementRecord&,
    const storage::OrchardStoredState&, const storage::OrchardCommitmentSets&,
    uint32_t catalog_activation_height, const storage::catalog::State* parent_catalog);
} // namespace dinero::consensus
