#pragma once

#include <cstdint>
#include <optional>

namespace dinero::daemon {

// A connected block is announced (cmpctblock/inv) only when it is near the best
// known header. A node catching up (a phone replaying from a snapshot, a fresh
// node in IBD) would otherwise push an announcement for every historical block
// it connects, flooding peers that already have them. IsInIBD() cannot decide
// this: it is false whenever an AssumeUTXO snapshot is loaded.
constexpr uint32_t kMaxAnnounceLagBlocks = 6;

constexpr bool ShouldAnnounceConnectedBlock(uint32_t connected_height,
                                            std::optional<uint32_t> best_header_height) {
    return !best_header_height || *best_header_height <= connected_height + kMaxAnnounceLagBlocks;
}

} // namespace dinero::daemon
