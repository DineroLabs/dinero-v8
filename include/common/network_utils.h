#pragma once

#include <string>

namespace dinero {

// Network validation
bool isValidIpAddress(const std::string& ip);
bool isValidPort(int port);

// Hostname resolution
std::string resolveHostname(const std::string& hostname);

// Network connectivity
bool isPortOpen(const std::string& host, int port, int timeout_seconds = 5);

// Address utilities
std::string getLocalIpAddress();
std::string formatAddress(const std::string& host, int port);
bool parseAddress(const std::string& address, std::string& host, int& port);

// Network interface utilities
std::string getNetworkInterface();

// Address classification
bool isLocalAddress(const std::string& address);

// Hostname sanitization
std::string sanitizeHostname(const std::string& hostname);

} // namespace dinero 