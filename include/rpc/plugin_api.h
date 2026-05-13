#pragma once

/**
 * Dinero RPC Plugin API v1.0
 *
 * Allows third-party modules to register RPC methods at runtime.
 * Plugins are loaded as shared libraries (.so, .dylib, .dll).
 *
 * Plugin Lifecycle:
 * 1. Daemon calls plugin_init() on load
 * 2. Plugin registers methods via registerHandler()
 * 3. Plugin methods callable via all RPC transports
 * 4. Daemon calls plugin_shutdown() on exit
 *
 * Example Plugin:
 * ```cpp
 * extern "C" {
 *   DINERO_PLUGIN_EXPORT int plugin_init(DineroPluginApi* api) {
 *     api->registerHandler("myplugin.hello", my_handler, "myplugin");
 *     return 0;
 *   }
 *   DINERO_PLUGIN_EXPORT void plugin_shutdown() {
 *     // Cleanup
 *   }
 * }
 * ```
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plugin API version
#define DINERO_PLUGIN_API_VERSION 1

// Export macro for plugin functions
#if defined(_WIN32)
  #define DINERO_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define DINERO_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// Forward declarations
struct DineroPluginApi;
struct ExecutionContext;
typedef struct Json_Value Json_Value;

/**
 * RPC handler function signature
 *
 * @param ctx Execution context (auth, client info, etc.)
 * @param params Input parameters as JSON
 * @return Result as JSON (plugin must allocate, daemon will free)
 */
typedef Json_Value* (*DineroRpcHandler)(
    const struct ExecutionContext* ctx,
    const Json_Value* params
);

/**
 * Logger function for plugins
 */
typedef void (*DineroLogFunc)(int level, const char* message);

/**
 * Plugin API structure
 *
 * Passed to plugin_init(). Plugins use this to interact with daemon.
 */
struct DineroPluginApi {
    uint32_t api_version;  // DINERO_PLUGIN_API_VERSION

    // RPC registration
    int (*registerHandler)(
        const char* method_name,
        DineroRpcHandler handler,
        const char* category
    );

    // Logging
    DineroLogFunc log_info;
    DineroLogFunc log_warn;
    DineroLogFunc log_error;

    // JSON helpers
    Json_Value* (*json_parse)(const char* str);
    char* (*json_stringify)(const Json_Value* value);
    void (*json_free)(Json_Value* value);

    // Daemon info
    const char* (*get_version)(void);
    const char* (*get_network)(void);  // "mainnet", "testnet", "regtest"
    uint64_t (*get_block_height)(void);

    // Reserved for future expansion
    void* reserved[8];
};

/**
 * Plugin initialization function
 *
 * Called when plugin is loaded. Must register all RPC methods.
 *
 * @param api Plugin API structure
 * @return 0 on success, non-zero on error
 */
DINERO_PLUGIN_EXPORT int plugin_init(struct DineroPluginApi* api);

/**
 * Plugin shutdown function
 *
 * Called when daemon exits. Plugin should cleanup resources.
 */
DINERO_PLUGIN_EXPORT void plugin_shutdown(void);

/**
 * Plugin metadata function (optional)
 *
 * Returns plugin info as JSON string.
 *
 * Example:
 * ```json
 * {
 *   "name": "my-plugin",
 *   "version": "1.0.0",
 *   "author": "John Doe",
 *   "description": "Custom RPC methods",
 *   "methods": ["myplugin.hello", "myplugin.status"]
 * }
 * ```
 */
DINERO_PLUGIN_EXPORT const char* plugin_metadata(void);

#ifdef __cplusplus
}
#endif

/**
 * C++ Plugin Helper Macros
 */
#ifdef __cplusplus

#include "din_json.h"
#include "rpc/rpc_registry.h"

// C++ wrapper for easier plugin development
#define DINERO_PLUGIN_BEGIN(plugin_name) \
namespace plugin_name { \
static DineroPluginApi* g_api = nullptr; \
static din::Json handler_wrapper(const ExecutionContext& ctx, const din::Json& params);

#define DINERO_PLUGIN_METHOD(method_name, category) \
static din::Json method_name##_impl(const ExecutionContext& ctx, const din::Json& params); \
static void register_##method_name() { \
    g_api->registerHandler(#method_name, \
        [](const ExecutionContext* ctx, const Json_Value* params) -> Json_Value* { \
            din::Json result = method_name##_impl(*ctx, *((din::Json*)params)); \
            return new Json_Value(result); \
        }, category); \
} \
static din::Json method_name##_impl(const ExecutionContext& ctx, const din::Json& params)

#define DINERO_PLUGIN_INIT() \
extern "C" DINERO_PLUGIN_EXPORT int plugin_init(DineroPluginApi* api) { \
    g_api = api;

#define DINERO_PLUGIN_END() \
    return 0; \
} \
extern "C" DINERO_PLUGIN_EXPORT void plugin_shutdown() {} \
}

#endif // __cplusplus
