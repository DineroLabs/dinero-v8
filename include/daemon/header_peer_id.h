#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dinero::daemon {

// HeaderSyncManager uses a numeric peer key while P2PManager uses an address.
// Keep the conversion in one place so connect, message, and disconnect paths
// always refer to the same state-machine entry.
inline uint64_t HeaderPeerId(const std::string& peer_address) {
    return std::hash<std::string>{}(peer_address);
}

} // namespace dinero::daemon
