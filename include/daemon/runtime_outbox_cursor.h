#pragma once
#include "primitives/uint256.h"
#include <cstdint>

namespace dinero {
// Local delivery position, available independently of the Orchard backend.
struct RuntimeOutboxCursor {
    uint64_t sequence = 0;
    uint256 digest;
    bool operator==(const RuntimeOutboxCursor&) const = default;
};
} // namespace dinero
