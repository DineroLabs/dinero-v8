#pragma once
#include <string>
#include <optional>

namespace din::net {

struct ProxyConfig {
  std::string socks5_host;
  std::string socks5_port;
  bool resolve_via_proxy{false};
  bool tor_only_onion{false};
  bool require_proxy_for_coinjoin{true};
  bool require_proxy_for_payjoin{false};
};

class ProxyManager {
public:
  explicit ProxyManager(const ProxyConfig& config);
  
  // Configuration
  const ProxyConfig& getConfig() const { return config_; }
  void updateConfig(const ProxyConfig& new_config) { config_ = new_config; }
  
  // Proxy availability
  bool isProxyAvailable() const;
  bool shouldUseProxy(const std::string& hostname) const;
  bool isOnionAddress(const std::string& hostname) const;
  
  // Health check
  bool performHealthCheck() const;
  
  // Configuration helpers
  static ProxyConfig fromConfigFile(const std::string& config_path);
  static ProxyConfig defaultConfig();

private:
  ProxyConfig config_;
  
  // Helper methods
  bool isLocalhost(const std::string& hostname) const;
  bool isPrivateNetwork(const std::string& hostname) const;
};

} // namespace din::net
