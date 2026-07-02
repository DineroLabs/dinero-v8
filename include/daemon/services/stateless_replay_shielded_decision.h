#pragma once

#include <cstdint>

namespace dinero {

// Pure marker-guard decision for the stateless replay/recovery shielded apply
// (see ChainstateService::ApplyStatelessReplayShielded). Kept as a free,
// header-only function of the two heights so it is trivially unit-testable
// without standing up a BlockValidator + ChainDB marker harness.
//
//   marker_height == block_height - 1 -> Apply   (pool sits exactly at the
//                                                  parent; advance it by one)
//   marker_height >= block_height      -> Skip    (already at/ahead — a second
//                                                  apply would double-count)
//   marker_height <  block_height - 1  -> GapFail (a hole between the pool and
//                                                  the block; contiguous-recovery
//                                                  invariant is broken — loud fail)
//
// The comparisons are ordered so `block_height - 1` is only evaluated once
// `marker_height < block_height` is established (hence `block_height >= 1`),
// which keeps the unsigned subtraction from underflowing. At block_height == 0
// every marker_height >= 0 satisfies the first branch, so the result is Skip
// (genesis carries no shielded activity to replay).
enum class StatelessReplayShieldedAction { Apply, Skip, GapFail };

inline StatelessReplayShieldedAction StatelessReplayShieldedDecision(
    uint32_t marker_height, uint32_t block_height) {
    if (marker_height >= block_height) {
        return StatelessReplayShieldedAction::Skip;
    }
    // marker_height < block_height here, so block_height >= 1 and the
    // subtraction below cannot underflow.
    if (marker_height == block_height - 1) {
        return StatelessReplayShieldedAction::Apply;
    }
    return StatelessReplayShieldedAction::GapFail;
}

}  // namespace dinero
