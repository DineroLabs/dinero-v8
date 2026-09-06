// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license.
#pragma once

#include <algorithm>

namespace dinero::qt {

// P2P telemetry cannot prove a remote node's active validated tip. Keep block
// and header observations separate so header progress cannot masquerade as
// remote block validation in the UI.
inline int peerBlocksSeen(int start_height, int synced_blocks) {
    return std::max(start_height, synced_blocks);
}

inline int peerHeadersSeen(int start_height, int synced_headers, int best_known) {
    return std::max({start_height, synced_headers, best_known});
}

} // namespace dinero::qt
