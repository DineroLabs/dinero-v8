#include "net/proxy_manager.h"
#include "net/socks5.h"
#include <boost/asio.hpp>
#include <fstream>
#include <regex>

namespace din::net {

ProxyManager::ProxyManager(const ProxyConfig& config) : config_(config) {}

bool ProxyManager::isProxyAvailable() const {
  return !config_.socks5_host.empty() && !config_.socks5_port.empty();
}

bool ProxyManager::shouldUseProxy(const std::string& hostname) const {
  if (!isProxyAvailable()) return false;
  
  // Don't proxy localhost or private networks
  if (isLocalhost(hostname) || isPrivateNetwork(hostname)) {
    return false;
  }
  
  // If tor_only_onion is enabled, only proxy .onion addresses
  if (config_.tor_only_onion) {
    return isOnionAddress(hostname);
  }
  
  // Otherwise, proxy all external addresses
  return true;
}

bool ProxyManager::isOnionAddress(const std::string& hostname) const {
  return hostname.length() > 6 && hostname.substr(hostname.length() - 6) == ".onion";
}

bool ProxyManager::performHealthCheck() const {
  if (!isProxyAvailable()) return false;
  
  try {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket socket(ioc);
    
    // Try to connect to the proxy
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(config_.socks5_host, config_.socks5_port);
    boost::asio::connect(socket, endpoints);
    
    // Try a simple SOCKS5 handshake
    std::vector<uint8_t> req{0x05, 0x01, 0x00};
    boost::asio::write(socket, boost::asio::buffer(req));
    
    uint8_t rep[2];
    boost::asio::read(socket, boost::asio::buffer(rep, 2));
    
    socket.close();
    return rep[0] == 0x05 && rep[1] == 0x00;
  } catch (const std::exception&) {
    return false;
  }
}

ProxyConfig ProxyManager::fromConfigFile(const std::string& config_path) {
  ProxyConfig config = defaultConfig();
  
  std::ifstream file(config_path);
  if (!file.is_open()) return config;
  
  std::string line;
  while (std::getline(file, line)) {
    // Simple INI-style parsing
    if (line.empty() || line[0] == '#') continue;
    
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos) continue;
    
    std::string key = line.substr(0, eq_pos);
    std::string value = line.substr(eq_pos + 1);
    
    // Trim whitespace
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);
    
    if (key == "proxy.socks5") {
      auto colon_pos = value.find(':');
      if (colon_pos != std::string::npos) {
        config.socks5_host = value.substr(0, colon_pos);
        config.socks5_port = value.substr(colon_pos + 1);
      }
    } else if (key == "proxy.resolve_via_proxy") {
      config.resolve_via_proxy = (value == "true" || value == "1");
    } else if (key == "tor.only_onion") {
      config.tor_only_onion = (value == "true" || value == "1");
    } else if (key == "coinjoin.require_proxy") {
      config.require_proxy_for_coinjoin = (value == "true" || value == "1");
    } else if (key == "payjoin.require_proxy") {
      config.require_proxy_for_payjoin = (value == "true" || value == "1");
    }
  }
  
  return config;
}

ProxyConfig ProxyManager::defaultConfig() {
  ProxyConfig config;
  config.socks5_host = "127.0.0.1";
  config.socks5_port = "9050";
  config.resolve_via_proxy = true;
  config.tor_only_onion = false;
  config.require_proxy_for_coinjoin = true;
  config.require_proxy_for_payjoin = false;
  return config;
}

bool ProxyManager::isLocalhost(const std::string& hostname) const {
  return hostname == "localhost" || hostname == "127.0.0.1" || hostname == "::1";
}

bool ProxyManager::isPrivateNetwork(const std::string& hostname) const {
  // Simple check for private network ranges
  std::regex private_regex(R"(^(10\.|172\.(1[6-9]|2[0-9]|3[01])\.|192\.168\.))");
  return std::regex_match(hostname, private_regex);
}

} // namespace din::net
