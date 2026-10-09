#pragma once
#include "consensus/orchard_candidate_coin_view.h"
#include "storage/orchard_catalog_state.h"

namespace dinero::consensus {
// Resolve candidate metadata from a completed parent catalog and authenticate
// exact external leaves against its stump. The caller must own the selected
// catalog record, header and profile; a decoded record alone is not authority.
// The reader serves immutable nodes at the captured roots. Provisional peer
// metadata never escapes before the complete candidate proof has passed.
// No full coin database, forest, optional transaction index, writes or live
// publication are used here. Canonical selection must still be rechecked.
[[nodiscard]] OrchardCandidateCoinView CaptureOrchardCatalogCoins(
    const OrchardBlockCandidate&, const OrchardBlockContext&, const BlockHeader&,
    const storage::catalog::State&, const storage::catalog::Tree::Read&);
}
