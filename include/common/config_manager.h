#pragma once

#include <string>
#include <cstdint>
#include <map>
#include <vector>
#include <algorithm>

namespace dinero {

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();
    
    bool loadConfig(const std::string& filename);
    bool saveConfig(const std::string& filename);
    
    std::string get(const std::string& key, const std::string& default_value = "") const;
    int getInt(const std::string& key, int default_value = 0) const;
    bool getBool(const std::string& key, bool default_value = false) const;
    
    // Helper methods for P2P configuration
    std::vector<std::pair<std::string, uint16_t>> getSeedNodes() const;
    
    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setBool(const std::string& key, bool value);
    
    bool has(const std::string& key) const;
    void clear();
    
private:
    std::map<std::string, std::string> config_;
    std::map<std::string, std::string> defaults_;
    
    std::string trim(const std::string& str) const;
};

// Global config manager instance
extern ConfigManager g_config;

} // namespace dinero 