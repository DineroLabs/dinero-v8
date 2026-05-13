/**
 * Example Dinero RPC Plugin
 *
 * Demonstrates how to create a runtime-loadable RPC extension.
 *
 * Build:
 *   clang++ -shared -fPIC -I../include example_plugin.cpp -o example_plugin.so
 *
 * Load:
 *   dinerod --plugin-dir=./plugins
 *   dinero-cli myplugin.hello '{"name": "Alice"}'
 */

#include "rpc/plugin_api.h"
#include <string.h>
#include <stdio.h>

static DineroPluginApi* g_api = nullptr;

// Helper to create JSON string response
static Json_Value* create_json_response(const char* key, const char* value) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "{\"%s\": \"%s\"}", key, value);
    return g_api->json_parse(buffer);
}

/**
 * myplugin.hello - Say hello to someone
 */
static Json_Value* rpc_hello(const ExecutionContext* ctx, const Json_Value* params) {
    // Parse name from params (simplified - real plugin should use JSON library)
    const char* name = "World";  // default

    g_api->log_info(0, "Hello RPC called");

    char message[256];
    snprintf(message, sizeof(message), "Hello, %s! Plugin is working.", name);

    return create_json_response("message", message);
}

/**
 * myplugin.status - Get plugin status
 */
static Json_Value* rpc_status(const ExecutionContext* ctx, const Json_Value* params) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
        "{\"plugin\": \"example\", "
        "\"version\": \"1.0.0\", "
        "\"daemon_version\": \"%s\", "
        "\"network\": \"%s\", "
        "\"block_height\": %llu}",
        g_api->get_version(),
        g_api->get_network(),
        (unsigned long long)g_api->get_block_height());

    return g_api->json_parse(buffer);
}

/**
 * myplugin.echo - Echo back the parameters
 */
static Json_Value* rpc_echo(const ExecutionContext* ctx, const Json_Value* params) {
    // Just return the input params as-is
    char* params_str = g_api->json_stringify(params);
    Json_Value* result = g_api->json_parse(params_str);
    free(params_str);
    return result;
}

/**
 * Plugin initialization
 */
extern "C" DINERO_PLUGIN_EXPORT int plugin_init(DineroPluginApi* api) {
    g_api = api;

    // Verify API version
    if (api->api_version != DINERO_PLUGIN_API_VERSION) {
        fprintf(stderr, "Plugin API version mismatch: expected %d, got %d\n",
                DINERO_PLUGIN_API_VERSION, api->api_version);
        return -1;
    }

    api->log_info(0, "Example plugin initializing...");

    // Register RPC methods
    api->registerHandler("myplugin.hello", rpc_hello, "plugin");
    api->registerHandler("myplugin.status", rpc_status, "plugin");
    api->registerHandler("myplugin.echo", rpc_echo, "plugin");

    api->log_info(0, "Example plugin loaded: 3 methods registered");

    return 0;
}

/**
 * Plugin shutdown
 */
extern "C" DINERO_PLUGIN_EXPORT void plugin_shutdown() {
    if (g_api) {
        g_api->log_info(0, "Example plugin shutting down");
    }
}

/**
 * Plugin metadata (optional)
 */
extern "C" DINERO_PLUGIN_EXPORT const char* plugin_metadata() {
    return "{"
           "\"name\": \"example-plugin\","
           "\"version\": \"1.0.0\","
           "\"author\": \"Dinero Team\","
           "\"description\": \"Example runtime-loadable RPC plugin\","
           "\"methods\": [\"myplugin.hello\", \"myplugin.status\", \"myplugin.echo\"]"
           "}";
}
