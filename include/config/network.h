#pragma once
#include <string>

namespace dinero {

// Network configuration constants
extern const std::string HRP;  // Human-readable part for Bech32 addresses

// Network parameters
extern const uint32_t COIN_TYPE;  // BIP44 coin type
extern const uint32_t NETWORK_VERSION;  // Network version

} // namespace dinero
