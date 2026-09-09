#pragma once
#include <cstdint>
#include <limits>

namespace dinero::consensus::shielded {
// Wait until the reset block is committed. Merely targeting height H in the
// mempool at H-1 is insufficient: the wallet must first observe the new epoch.
inline bool WalletAuthEpochReady(uint32_t tip, uint32_t auth, uint32_t reset) {
    return auth != std::numeric_limits<uint32_t>::max() && auth == reset && tip >= auth;
}
}
