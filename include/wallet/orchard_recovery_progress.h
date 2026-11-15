#pragma once
#include <cstdint>
#include "primitives/uint256.h"

namespace dinero::wallet {
// Observation values remain available when the Orchard runtime is disabled.
struct OrchardAccount0RecoveryProgress {
    bool complete=false,already_recovered=false;
    uint64_t applied=0,target=0;
    uint32_t height=0;
    uint256 block;
    // Transport observations are NOT wallet-applied sequence/checkpoint data.
    bool history_requested=false,history_ready=false;
    // Recovered account0 with no selected history source yet (not converged,
    // or the selected source is unavailable). Reported, never an error.
    bool awaiting_history_source=false;
    uint32_t history_verified_height=0,history_target_height=0;
    uint256 history_target_hash;
};
}
