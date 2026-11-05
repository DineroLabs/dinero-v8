#pragma once

#include <cstdint>
#include <limits>

namespace dinero::consensus {

// A stricter first-transaction shape changes block validity. Keep the
// historical rule until a network selects and coordinates an activation.
inline bool IsCoinbaseStructureActive(uint64_t height, uint32_t activation_height) {
    return activation_height != std::numeric_limits<uint32_t>::max() &&
           height >= activation_height;
}

} // namespace dinero::consensus
