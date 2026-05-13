// DEAD CODE — not compiled (not in CMakeLists.txt).
// Production cookie generation uses rpc_auth.cpp with SecureRandom CSPRNG.
// Kept for reference only. Do not rely on this code.

#include "daemon/secure_defaults.h"
#include "common/logger.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <pwd.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#endif

namespace dinero {

std::string SecureDefaults::getDefaultDataDir() {
#ifdef _WIN32
    // Windows: %APPDATA%\Dinero
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata) {
        std::string result = std::string(appdata) + "\\Dinero";
        free(appdata);
        return result;
    }
    return "C:\\Users\\" + std::string(getenv("USERNAME")) + "\\AppData\\Roaming\\Dinero";
#elif defined(__APPLE__)
    // macOS: ~/Library/Application Support/Dinero
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/Library/Application Support/Dinero";
#else
    // Linux: $XDG_DATA_HOME/Dinero or ~/.local/share/Dinero
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home) {
        return std::string(xdg_data_home) + "/Dinero";
    }
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/.local/share/Dinero";
#endif
}

std::string SecureDefaults::getDefaultConfigDir() {
#ifdef _WIN32
    // Windows: same as datadir
    return getDefaultDataDir();
#elif defined(__APPLE__)
    // macOS: same as datadir
    return getDefaultDataDir();
#else
    // Linux: $XDG_CONFIG_HOME/Dinero or ~/.config/Dinero
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home) {
        return std::string(xdg_config_home) + "/Dinero";
    }
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/.config/Dinero";
#endif
}

bool SecureDefaults::createSecureDirectories(const std::string& datadir, const std::string& configdir) {
    try {
        // Create datadir
        std::filesystem::create_directories(datadir);
        
        // Create configdir
        std::filesystem::create_directories(configdir);
        
        // Set secure permissions (Unix-like systems)
#ifndef _WIN32
        std::filesystem::permissions(datadir, std::filesystem::perms::owner_all);
        std::filesystem::permissions(configdir, std::filesystem::perms::owner_all);
#endif
        
        dinero::g_logger.info("Created secure directories: " + datadir + ", " + configdir);
        return true;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to create directories: " + std::string(e.what()));
        return false;
    }
}

bool SecureDefaults::writeSecureConfig(const std::string& config_path, const std::string& datadir) {
    try {
        std::ofstream config(config_path);
        if (!config.is_open()) {
            dinero::g_logger.error("Failed to open config file: " + config_path);
            return false;
        }
        
        // Write secure defaults
        config << "# Dinero Core Configuration\n";
        config << "# Generated with secure defaults\n\n";
        config << "server=1\n";
        config << "rpcbind=127.0.0.1\n";
        config << "rpcallowip=127.0.0.1\n";
        config << "rpcport=20998\n";
        config << "port=20999\n";
        config << "rpccookiefile=" << datadir << "/.cookie\n";
        config << "\n# Security: localhost-only RPC binding\n";
        config << "# Cookie authentication enabled\n";
        config << "# No external network access by default\n";
        
        config.close();
        
        // Set secure permissions
#ifndef _WIN32
        std::filesystem::permissions(config_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
        
        dinero::g_logger.info("Created secure config: " + config_path);
        return true;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to write config: " + std::string(e.what()));
        return false;
    }
}

bool SecureDefaults::setupSecureEnvironment(const std::string& datadir) {
    try {
        // Create .cookie file with secure permissions
        std::string cookie_path = datadir + "/.cookie";
        std::ofstream cookie(cookie_path);
        if (!cookie.is_open()) {
            dinero::g_logger.error("Failed to create cookie file: " + cookie_path);
            return false;
        }
        
        // Generate random cookie content (simplified for demo)
        cookie << "__cookie__:";
        for (int i = 0; i < 32; ++i) {
            cookie << "0123456789abcdef"[rand() % 16];
        }
        cookie.close();
        
        // Set secure permissions
#ifndef _WIN32
        std::filesystem::permissions(cookie_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
        
        dinero::g_logger.info("Created secure cookie file: " + cookie_path);
        return true;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to setup secure environment: " + std::string(e.what()));
        return false;
    }
}

} // namespace dinero
