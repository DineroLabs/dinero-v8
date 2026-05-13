#pragma once

#include "rpc/plugin_api.h"
#include <string>
#include <vector>
#include <memory>

namespace dinero {
namespace rpc {

class RpcRegistry;

/**
 * Plugin loader for dynamic RPC extensions
 *
 * Loads shared libraries (.so/.dylib/.dll) and registers their RPC methods.
 * Plugins must implement the plugin_init() and plugin_shutdown() functions.
 */
class PluginLoader {
public:
    explicit PluginLoader(RpcRegistry* registry);
    ~PluginLoader();

    /**
     * Load a plugin from file
     *
     * @param path Path to shared library (.so, .dylib, .dll)
     * @return true on success, false on error
     */
    bool loadPlugin(const std::string& path);

    /**
     * Load all plugins from directory
     *
     * Searches for *.so (Linux), *.dylib (macOS), *.dll (Windows) files.
     *
     * @param dir_path Directory to scan
     * @return Number of plugins loaded
     */
    int loadPluginsFromDirectory(const std::string& dir_path);

    /**
     * Unload all plugins
     *
     * Calls plugin_shutdown() for each loaded plugin.
     */
    void unloadAll();

    /**
     * Get list of loaded plugins
     *
     * @return Vector of plugin paths
     */
    std::vector<std::string> getLoadedPlugins() const;

    /**
     * Get plugin info (from plugin_metadata() function)
     *
     * @param path Plugin path
     * @return JSON metadata or null if unavailable
     */
    std::string getPluginMetadata(const std::string& path) const;

private:
    struct PluginHandle {
        void* handle;           // dlopen handle
        std::string path;
        DineroPluginApi api;
    };

    RpcRegistry* registry_;
    std::vector<std::unique_ptr<PluginHandle>> plugins_;

    // Populate plugin API structure
    DineroPluginApi createPluginApi();

    // Static callback functions for plugin API
    static int api_registerHandler(const char* method, DineroRpcHandler handler, const char* category);
    static void api_log_info(int level, const char* message);
    static void api_log_warn(int level, const char* message);
    static void api_log_error(int level, const char* message);
    static const char* api_get_version();
    static const char* api_get_network();
    static uint64_t api_get_block_height();

    // Global registry pointer for callbacks (thread-local would be better in production)
    static RpcRegistry* s_registry;
};

} // namespace rpc
} // namespace dinero
