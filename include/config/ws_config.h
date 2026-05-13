#pragma once
#include <string>

namespace dinero {
namespace config {

struct WsConfig {
    std::string bind;   // "127.0.0.1" | "0.0.0.0" | specific IP
    int         port;   // 18332 default
    std::string path;   // "/ws" default; always leading '/'
};

/**
 * Normalize WebSocket path to ensure it starts with '/'
 */
std::string normalizePath(const std::string& p);

/**
 * Load WebSocket configuration from command line arguments
 */
WsConfig LoadWsConfig();

} // namespace config
} // namespace dinero
