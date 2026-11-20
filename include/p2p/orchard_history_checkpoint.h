#pragma once
// When does an Orchard wallet history that was verified up to one block still
// stand, now that the selected tip may be another?
//
// The history download verified every block body from activation to a target
// and checked the result against that target's catalog commitment. That
// verification is about those blocks. A later block that extends the same
// chain does not undo it; a reorganisation that replaces the target does.
//
// So a verified target is a checkpoint: it stands while it is the current
// target, or an ancestor of it on the selected header chain with the same
// snapshot base and activation. A wallet builds on the checkpoint and reads
// the blocks after it from the node's own record, instead of waiting for a
// moment at which no block has arrived since verification ended.

#include "p2p/orchard_phone_history_target.h"

#include <optional>

namespace dinero::p2p {

// `ancestor_at(tip_hash, height)` is the hash of the block at `height` on the
// chain ending at `tip_hash` (the selected header chain), or nullopt when it
// cannot be told. Anything that cannot be told does not stand.
template <class AncestorAt>
bool OrchardHistoryCheckpointStands(const OrchardPhoneHistoryTarget& verified, const OrchardPhoneHistoryTarget& current,
                                    AncestorAt&& ancestor_at) {
    if (verified == current) return true;
    if (verified.base != current.base || verified.activation != current.activation) return false;  // another snapshot or network rule
    if (current.height <= verified.height) return false;  // same height, other block; or a shorter chain: a reorganisation
    const std::optional<uint256> at = ancestor_at(current.hash, verified.height);
    return at && *at == verified.hash;
}

}  // namespace dinero::p2p
