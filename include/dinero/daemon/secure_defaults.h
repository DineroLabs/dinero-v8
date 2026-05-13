#pragma once
#include <string>

namespace dinero {

class SecureDefaults {
public:
    // Get platform-specific default data directory
    static std::string getDefaultDataDir();
    
    // Get platform-specific default config directory
    static std::string getDefaultConfigDir();
    
    // Create secure directories with proper permissions
    static bool createSecureDirectories(const std::string& datadir, const std::string& configdir);
    
    // Write secure configuration file with safe defaults
    static bool writeSecureConfig(const std::string& config_path, const std::string& datadir);
    
    // Setup secure environment (cookie file, permissions)
    static bool setupSecureEnvironment(const std::string& datadir);
};

} // namespace dinero
