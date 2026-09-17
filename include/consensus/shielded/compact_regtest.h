#pragma once
// Isolated regtest experiment; no production-network format assignment.
#include "consensus/shielded/shielded_tx.h"
#include "consensus/chainparams.h"

namespace dinero::consensus::shielded {
struct CompactRegtestRules {
    bool regtest = false;
    uint32_t activation_height = UINT32_MAX;
    bool Active(uint64_t height) const {
#ifdef DINERO_ENABLE_COMPACT_REGTEST
        return regtest && activation_height != UINT32_MAX && height >= activation_height;
#else
        return false;
#endif
    }
};
inline CompactRegtestRules CompactRulesFor(const ChainParams& params) {
    return {params.name == "regtest", params.shielded_compact_regtest_activation_height};
}

#ifdef DINERO_ENABLE_COMPACT_REGTEST
// Transactional transforms: failure leaves the destination unchanged. Circuit
// selection is fixed by the ordinary Auth profile, never by wire dimensions.
bool PackCompactRegtestBundle(ShieldedBundle& bundle);
bool ExpandCompactRegtestBundle(const ShieldedBundle& bundle, ShieldedBundle& expanded);
#endif
} // namespace dinero::consensus::shielded
