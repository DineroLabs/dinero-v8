#pragma once
#include "primitives/block.h"
#include <cstdint>
#include <vector>

namespace dinero::daemon {
// Decode the existing headers-message framing; consensus validation follows
// in HeaderSyncP2P. Does not validate PoW or Utreexo commitments.
std::vector<BlockHeader> ParseHeadersPayload(const std::vector<uint8_t>& payload);
}
