#pragma once

#include "consensus/outpoint.h"
#include <array>
#include <vector>

namespace dinero {

// Mempool conflict identities of an already validated block. This type does
// not assert that a parsed body has passed consensus checks.
struct ConnectedBlockEffects {
    std::vector<uint256> confirmed_txids;
    std::vector<OutPoint> spent_transparent_inputs;
    // Orchard action nullifiers only; never legacy shielded nullifiers.
    std::vector<std::array<uint8_t, 32>> orchard_nullifiers;
};

} // namespace dinero
