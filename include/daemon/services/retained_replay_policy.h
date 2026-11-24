#pragma once
// What to do with the retained phone account replay when it cannot be
// extended because another reader still holds it
// (ChainstateService::getRuntimeAccountReplay, the verified-history path).
//
// The retained view is the product of a full build from the verified history
// checkpoint: minutes on a desktop, about seven on a phone. It used to be
// dropped whenever a block arrived while another reader held it: the view
// could not be extended (ExtendTail needs the sole owner), was no longer at
// the head, and so was thrown away and built again from the start. A wallet
// being recovered reads the view on every delivery pass and from its own
// requests, so on a chain with blocks arriving a phone rebuilt for ever and
// its wallet never finished recovery.
//
// The view is still a verified prefix of the node's record. It is kept, the
// caller is told to come back (the refusal is retryable and every caller on
// this path already retries), and the next caller that finds it free extends
// it by the blocks that arrived. Only a holder that never lets go, counted in
// consecutive refusals, ends in the old behaviour.

#include <cstdint>

namespace dinero {

enum class RetainedReplayAction {
    ReturnCurrent,   // it is still at the head: hand it out as it is
    KeepAndRetry,    // held and behind the head: keep it, the caller retries
    DropAndRebuild,  // held for too long: give it up and build a new one
};

inline constexpr uint32_t kRetainedReplayHeldRefusals = 120;

// `at_head`: the held view passed the adoption checks as it is.
// `held_refusals`: consecutive earlier answers of KeepAndRetry.
inline RetainedReplayAction WhenRetainedReplayIsHeld(bool at_head, uint32_t held_refusals,
                                                     uint32_t limit = kRetainedReplayHeldRefusals) {
    if (at_head) return RetainedReplayAction::ReturnCurrent;
    return held_refusals < limit ? RetainedReplayAction::KeepAndRetry : RetainedReplayAction::DropAndRebuild;
}

// The refusal log says each reason once; a reason that keeps repeating is
// said again with its count at these points, so hours of the same refusal do
// not look like one line.
inline bool RepeatedRefusalIsLogged(uint64_t repeats) {
    return repeats == 10 || repeats == 100 || (repeats >= 1000 && repeats % 1000 == 0);
}

}  // namespace dinero
