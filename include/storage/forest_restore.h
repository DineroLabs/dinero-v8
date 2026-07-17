#ifndef DINERO_STORAGE_FOREST_RESTORE_H
#define DINERO_STORAGE_FOREST_RESTORE_H

// Forest checkpoint delta campaign phase 3
// (docs/design/forest-checkpoint-deltas.md).
//
// With full forest checkpoints written every N blocks, any consumer that
// needs the forest at a specific height rebuilds it as
//   nearest checkpoint at-or-below + forward replay of the per-block
//   UD:<blockhash> delta sidecars,
// verifying the forest root against every replayed block's header
// utreexo_root. Shared by ChainstateService's startup restore and
// BridgeNode's historical proof generation.

#include <cstdint>
#include <string>

#include "common/status.h"

namespace dinero {

class ChainDB;

namespace consensus {
class UtreexoForest;
}

namespace storage {

// Replays the UD sidecars for heights (from_exclusive, to_inclusive]
// onto `forest`, in ascending order, mirroring live validation exactly:
// canonical-roots fork flipped (+rebuildRoots) at its activation height,
// each block's delta applied in recorded two-pass order with position
// cross-checks, and the resulting root compared against the block
// header's utreexo_root. Any missing/corrupt sidecar, header, or root
// mismatch fails loudly; `forest` may then be partially advanced and
// must be discarded by the caller.
Status ReplayUtreexoDeltaRange(const ChainDB& db,
                               consensus::UtreexoForest& forest,
                               uint32_t from_exclusive,
                               uint32_t to_inclusive,
                               std::string& error);

// Rebuilds the forest state at exactly `target_height`: loads the nearest
// full checkpoint at-or-below, sets the canonical-roots flag per the
// checkpoint's height, and replays sidecars up to the target. On success
// `out` holds the verified forest; on failure `out` must be discarded.
Status RestoreHistoricalForest(const ChainDB& db, uint32_t target_height,
                               consensus::UtreexoForest& out,
                               std::string& error);

}  // namespace storage
}  // namespace dinero

#endif  // DINERO_STORAGE_FOREST_RESTORE_H
