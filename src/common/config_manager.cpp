#include "common/config_manager.h"
#include "common/logger.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>

namespace dinero {

ConfigManager::ConfigManager() {
    // Initialize with default values
    defaults_["rpc_host"] = "127.0.0.1";
    defaults_["rpc_port"] = "8332";
    defaults_["rpc_user"] = "dinero";
    defaults_["rpc_password"] = "";
    defaults_["stratum_port"] = "3333";
    defaults_["mining_threads"] = "1";
    defaults_["log_level"] = "INFO";
    defaults_["data_dir"] = "./data";
    
    // P2P Network Configuration. Defaults match Dinero mainnet
    // chainparams (port 20999, seed1-4.dinero-coin.com). Authoritative
    // source for seeds is src/consensus/chainparams_impl.cpp::vSeeds —
    // these defaults are a fallback for the ConfigManager API and must
    // stay in sync with that list.
    defaults_["p2p_port"] = "20999";
    defaults_["p2p_listen_address"] = "0.0.0.0";  // Listen on all interfaces
    defaults_["p2p_business_ip"] = "";  // Business IP address (to be configured)
    defaults_["p2p_home_ip"] = "";      // Home IP address (to be configured)
    defaults_["p2p_external_port"] = "20999";  // External port for port forwarding
    defaults_["p2p_max_connections"] = "125";
    defaults_["p2p_seed_nodes"] = "seed.dinerolabs.org:20999,seed2.dinerolabs.org:20999,seed3.dinerolabs.org:20999";
}

ConfigManager::~ConfigManager() {
    // Cleanup
}

bool ConfigManager::loadConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        g_logger.warning("Could not open config file: " + filename);
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Parse key=value pairs
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim whitespace
            key = trim(key);
            value = trim(value);
            
            config_[key] = value;
        }
    }
    
    g_logger.info("Loaded config from: " + filename);
    return true;
}

bool ConfigManager::saveConfig(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        g_logger.error("Could not create config file: " + filename);
        return false;
    }
    
    for (const auto& pair : config_) {
        file << pair.first << "=" << pair.second << std::endl;
    }
    
    g_logger.info("Saved config to: " + filename);
    return true;
}

std::string ConfigManager::get(const std::string& key, const std::string& default_value) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        return it->second;
    }
    
    // Check defaults
    auto default_it = defaults_.find(key);
    if (default_it != defaults_.end()) {
        return default_it->second;
    }
    
    return default_value;
}

int ConfigManager::getInt(const std::string& key, int default_value) const {
    std::string value = get(key, "");
    if (value.empty()) {
        return default_value;
    }
    
    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        g_logger.warning("Invalid integer value for key '" + key + "': " + value);
        return default_value;
    }
}

bool ConfigManager::getBool(const std::string& key, bool default_value) const {
    std::string value = get(key, "");
    if (value.empty()) {
        return default_value;
    }
    
    // Convert to lowercase for comparison
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    
    return (value == "true" || value == "1" || value == "yes" || value == "on");
}

void ConfigManager::set(const std::string& key, const std::string& value) {
    config_[key] = value;
}

void ConfigManager::setInt(const std::string& key, int value) {
    config_[key] = std::to_string(value);
}

void ConfigManager::setBool(const std::string& key, bool value) {
    config_[key] = value ? "true" : "false";
}

bool ConfigManager::has(const std::string& key) const {
    return config_.find(key) != config_.end();
}

void ConfigManager::clear() {
    config_.clear();
}

std::string ConfigManager::trim(const std::string& str) const {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::vector<std::pair<std::string, uint16_t>> ConfigManager::getSeedNodes() const {
    std::vector<std::pair<std::string, uint16_t>> nodes;
    
    // Get seed nodes from configuration
    std::string seed_nodes_str = get("p2p_seed_nodes");
    if (seed_nodes_str.empty()) {
        return nodes;
    }
    
    // Parse comma-separated list of host:port pairs
    std::istringstream iss(seed_nodes_str);
    std::string node;
    
    while (std::getline(iss, node, ',')) {
        node = trim(node);
        if (node.empty()) continue;
        
        // Split host:port
        size_t colon_pos = node.find(':');
        if (colon_pos != std::string::npos) {
            std::string host = trim(node.substr(0, colon_pos));
            std::string port_str = trim(node.substr(colon_pos + 1));
            
            try {
                uint16_t port = static_cast<uint16_t>(std::stoi(port_str));
                nodes.push_back({host, port});
            } catch (const std::exception&) {
                // Skip invalid port numbers
                continue;
            }
        } else {
            // Default port if not specified
            nodes.push_back({node, 8333});
        }
    }
    
    // Add business and home IPs if configured
    std::string business_ip = get("p2p_business_ip");
    std::string home_ip = get("p2p_home_ip");
    uint16_t external_port = static_cast<uint16_t>(getInt("p2p_external_port", 8333));
    
    if (!business_ip.empty()) {
        nodes.push_back({business_ip, external_port});
    }
    
    if (!home_ip.empty()) {
        nodes.push_back({home_ip, external_port});
    }
    
    return nodes;
}

// Global config manager instance
ConfigManager g_config;

} // namespace dinero
