#pragma once

#include "consensus/outpoint.h"
#include <vector>

namespace dinero {

// Transparent mempool effects of an already validated block. This type does
// not assert that a parsed body has passed consensus checks.
struct ConnectedBlockEffects {
    std::vector<uint256> confirmed_txids;
    std::vector<OutPoint> spent_transparent_inputs;
};

} // namespace dinero
