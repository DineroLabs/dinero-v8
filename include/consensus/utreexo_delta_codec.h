#ifndef DINERO_CONSENSUS_UTREEXO_DELTA_CODEC_H
#define DINERO_CONSENSUS_UTREEXO_DELTA_CODEC_H

// Utreexo per-block delta record codec + forward replay.
//
// The UD:<blockhash> sidecar has been written into ConnectTip's unified
// batch since the April 2026 P1 atomicity fix (undo/DisconnectTip
// consumer). The forest checkpoint delta campaign phase 2
// (docs/design/forest-checkpoint-deltas.md) makes it restore-critical:
// restart rebuilds the forest as latest-checkpoint + forward replay of
// these records. This unit hoists the codec out of chainstate_service.cpp
// so the husk/equivalence suites pin the production code, and adds the
// forward-apply used by the restore path.
//
// Wire format (schema v1, unchanged from the original file-static codec —
// every v8 datadir already contains records in this format):
//   u8 schema | u64 numLeavesBefore
//   | varint del_count | del_count × (32B leafHash, u64 position)
//   | varint add_count | add_count × (32B leafHash, u64 position)
// Deserialize is all-or-nothing: truncation, trailing bytes, or an
// unknown schema fail loudly and leave the out-parameter untouched.

#include <string>

#include "consensus/utreexo_delta.h"

namespace dinero {

class uint256;

namespace consensus {
class UtreexoForest;
}

std::string MakeUtreexoDeltaUndoKey(const uint256& block_hash);

bool SerializeUtreexoDelta(const consensus::UtreexoDelta& delta,
                           std::string& out, std::string& error);

bool DeserializeUtreexoDelta(const std::string& data,
                             consensus::UtreexoDelta& out, std::string& error);

// Re-applies one block's delta in the exact order live validation mutated
// the forest (ConnectBlockInternal two-pass: all recorded deletes in order,
// then all recorded adds in order). Every step is cross-checked against the
// record — pre-state leaf count, per-delete success at the recorded
// position, per-add position equality — so a mismatched forest/delta pair
// fails loudly instead of building a silently divergent forest. On failure
// the forest may be partially mutated; callers must discard it.
bool ApplyUtreexoDeltaForward(consensus::UtreexoForest& forest,
                              const consensus::UtreexoDelta& delta,
                              std::string& error);

}  // namespace dinero

#endif  // DINERO_CONSENSUS_UTREEXO_DELTA_CODEC_H
