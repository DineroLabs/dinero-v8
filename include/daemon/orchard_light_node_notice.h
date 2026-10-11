#pragma once
// Light (stateless Utreexo, CSN) nodes cannot follow the chain past Orchard
// activation yet: the compact Orchard path has no first enrollment, no way to
// obtain the activation state without full history, and is kept off at
// startup (storage::RequireFullOrchardStorage). Instead of stalling silently
// at the activation parent, such a node says so through its status RPCs and
// log. Empty when the node is not affected.
#include "consensus/chainparams.h"

#include <cstdint>
#include <string>

namespace dinero::daemon {

inline std::string OrchardLightNodeNotice(const ChainParams& params, bool stateless, uint32_t tip_height) {
    const uint32_t activation = params.orchard_activation_height;
    if (!stateless || activation == UINT32_MAX || activation == 0) return {};
    const std::string stop = std::to_string(activation - 1), at = std::to_string(activation);
    if (uint64_t(tip_height) + 1 < activation)
        return "This light (stateless) node will stop at block " + stop + ": it cannot follow the chain past "
               "Orchard activation at block " + at + ". Switch to a full node, or use the wallet's remote "
               "mode, before then.";
    return "This light (stateless) node stopped at block " + stop + ": it cannot follow the chain past "
           "Orchard activation at block " + at + ". Switch to a full node, or use the wallet's remote mode.";
}

}  // namespace dinero::daemon
