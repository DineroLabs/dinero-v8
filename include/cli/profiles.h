#pragma once
#include <string>
#include <optional>
#include <map>
#include "compat/jsoncpp_compat.h"

// Forward declaration
namespace dinero::cli {
    struct CliOverrides;
}

namespace dinero::cli {

struct Profile {
    std::string name;
    std::optional<std::string> network;
    std::optional<std::string> rpcUrl;
    std::optional<std::string> cookieFile;
    std::optional<std::string> datadir;
    std::optional<std::string> wallet;
    std::map<std::string, std::string> env; // Environment variables
};

class ProfileManager {
private:
    std::string configPath_;
    std::map<std::string, Profile> profiles_;
    
public:
    ProfileManager(const std::string& configPath = "");
    
    // Load profiles from config file
    bool loadProfiles();
    
    // Save profiles to config file
    bool saveProfiles();
    
    // Get profile by name
    std::optional<Profile> getProfile(const std::string& name) const;
    
    // Set/update profile
    void setProfile(const Profile& profile);
    
    // Delete profile
    bool deleteProfile(const std::string& name);
    
    // List all profile names
    std::vector<std::string> listProfiles() const;
    
    // Apply profile to CLI overrides
    void applyProfile(const std::string& profileName, CliOverrides& overrides) const;
    
private:
    std::string getDefaultConfigPath() const;
    Json::Value profileToJson(const Profile& profile) const;
    Profile profileFromJson(const std::string& name, const Json::Value& json) const;
};

} // namespace dinero::cli
