#include "config/network.h"
#include "consensus/coin_type.h"

namespace dinero {

// Network configuration constants
const std::string HRP = "din";  // Human-readable part for Bech32 addresses

// Network parameters  
const uint32_t COIN_TYPE = dinero::consensus::DINERO_COIN_TYPE;  // Official Dinero coin type
const uint32_t NETWORK_VERSION = 1;  // Network version

} // namespace dinero
