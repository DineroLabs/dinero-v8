#pragma once
#include <cstdint>

namespace PortUtil {
    // Find a free port starting from the suggested port
    // If suggested port is 0 or busy, returns a random free port
    uint16_t findFreePort(uint16_t suggested = 0);
    
    // Check if a specific port is available
    bool isPortFree(uint16_t port);
}
