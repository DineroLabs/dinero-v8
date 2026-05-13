#pragma once
#include <cstdint>

namespace dinero {
namespace net {

/**
 * Get the effective bound port for a socket file descriptor.
 * This is useful when binding to port 0 (auto-selection) to find
 * the actual port assigned by the OS.
 * 
 * @param fd Socket file descriptor
 * @return The actual bound port, or -1 on error
 */
int effective_bound_port(int fd);

} // namespace net
} // namespace dinero
