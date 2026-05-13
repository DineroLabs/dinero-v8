#pragma once

#include <string>

namespace dinero {
namespace cli {

/**
 * Extract host from RPC URL
 * @param rpcUrl Full RPC URL (e.g., "http://localhost:20998/")
 * @return Host part (e.g., "localhost")
 */
std::string extractHost(const std::string& rpcUrl);

/**
 * Extract port from RPC URL
 * @param rpcUrl Full RPC URL (e.g., "http://localhost:20998/")
 * @return Port part (e.g., "20998")
 */
std::string extractPort(const std::string& rpcUrl);

} // namespace cli
} // namespace dinero
