#include "cli/profiles.h"
#include "cli/overrides.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace dinero::cli {

ProfileManager::ProfileManager(const std::string& configPath) 
    : configPath_(configPath.empty() ? getDefaultConfigPath() : configPath) {}

std::string ProfileManager::getDefaultConfigPath() const {
    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE"); // Windows fallback
    if (!home) return "./dinero-cli-profiles.json";
    
    std::filesystem::path configDir = std::filesystem::path(home) / ".dinero-cli";
    std::filesystem::create_directories(configDir);
    return (configDir / "profiles.json").string();
}

bool ProfileManager::loadProfiles() {
    if (!std::filesystem::exists(configPath_)) {
        return true; // No config file is fine
    }
    
    std::ifstream file(configPath_);
    if (!file.is_open()) {
        return false;
    }
    
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    
    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        std::cerr << "Failed to parse profiles config: " << errors << std::endl;
        return false;
    }
    
    profiles_.clear();
    if (root.isObject() && root.isMember("profiles") && root["profiles"].isObject()) {
        for (const auto& name : root["profiles"].getMemberNames()) {
            profiles_[name] = profileFromJson(name, root["profiles"][name]);
        }
    }
    
    return true;
}

bool ProfileManager::saveProfiles() {
    Json::Value root(Json::objectValue);
    Json::Value profilesJson(Json::objectValue);
    
    for (const auto& [name, profile] : profiles_) {
        profilesJson[name] = profileToJson(profile);
    }
    
    root["profiles"] = profilesJson;
    root["version"] = "1.0";
    
    std::ofstream file(configPath_);
    if (!file.is_open()) {
        return false;
    }
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    
    return true;
}

std::optional<Profile> ProfileManager::getProfile(const std::string& name) const {
    auto it = profiles_.find(name);
    if (it != profiles_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ProfileManager::setProfile(const Profile& profile) {
    profiles_[profile.name] = profile;
}

bool ProfileManager::deleteProfile(const std::string& name) {
    return profiles_.erase(name) > 0;
}

std::vector<std::string> ProfileManager::listProfiles() const {
    std::vector<std::string> names;
    for (const auto& [name, profile] : profiles_) {
        names.push_back(name);
    }
    return names;
}

void ProfileManager::applyProfile(const std::string& profileName, CliOverrides& overrides) const {
    auto profile = getProfile(profileName);
    if (!profile) return;
    
    // Apply profile settings only if not already set by flags
    if (profile->network && !overrides.rpcUrl) {
        // Network implies default RPC URL patterns
    }
    if (profile->rpcUrl && !overrides.rpcUrl) {
        overrides.rpcUrl = profile->rpcUrl;
    }
    if (profile->cookieFile && !overrides.cookieFile) {
        overrides.cookieFile = profile->cookieFile;
    }
    if (profile->datadir && !overrides.datadir) {
        overrides.datadir = profile->datadir;
    }
    if (profile->wallet && !overrides.wallet) {
        overrides.wallet = profile->wallet;
    }
    
    // Set environment variables from profile
    for (const auto& [key, value] : profile->env) {
        if (!getenv(key.c_str())) { // Don't override existing env vars
            setenv(key.c_str(), value.c_str(), 0);
        }
    }
}

Json::Value ProfileManager::profileToJson(const Profile& profile) const {
    Json::Value json(Json::objectValue);
    
    if (profile.network) json["network"] = *profile.network;
    if (profile.rpcUrl) json["rpc_url"] = *profile.rpcUrl;
    if (profile.cookieFile) json["cookie_file"] = *profile.cookieFile;
    if (profile.datadir) json["datadir"] = *profile.datadir;
    if (profile.wallet) json["wallet"] = *profile.wallet;
    
    if (!profile.env.empty()) {
        Json::Value envJson(Json::objectValue);
        for (const auto& [key, value] : profile.env) {
            envJson[key] = value;
        }
        json["env"] = envJson;
    }
    
    return json;
}

Profile ProfileManager::profileFromJson(const std::string& name, const Json::Value& json) const {
    Profile profile;
    profile.name = name;
    
    if (json.isMember("network") && json["network"].isString()) {
        profile.network = json["network"].asString();
    }
    if (json.isMember("rpc_url") && json["rpc_url"].isString()) {
        profile.rpcUrl = json["rpc_url"].asString();
    }
    if (json.isMember("cookie_file") && json["cookie_file"].isString()) {
        profile.cookieFile = json["cookie_file"].asString();
    }
    if (json.isMember("datadir") && json["datadir"].isString()) {
        profile.datadir = json["datadir"].asString();
    }
    if (json.isMember("wallet") && json["wallet"].isString()) {
        profile.wallet = json["wallet"].asString();
    }
    
    if (json.isMember("env") && json["env"].isObject()) {
        for (const auto& key : json["env"].getMemberNames()) {
            if (json["env"][key].isString()) {
                profile.env[key] = json["env"][key].asString();
            }
        }
    }
    
    return profile;
}

} // namespace dinero::cli
