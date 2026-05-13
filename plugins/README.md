# DineroCoin RPC Plugins

Runtime-loadable extensions for the DineroCoin RPC system.

## Overview

Plugins allow third-party developers to add custom RPC methods without modifying the core daemon. Plugins are loaded as shared libraries at runtime.

## Features

- ✅ C and C++ support
- ✅ Full access to RPC registry
- ✅ Daemon info queries (version, network, block height)
- ✅ Logging integration
- ✅ JSON helpers
- ✅ All RPC transports (HTTP, WebSocket, CLI)

## Building a Plugin

### C Example

```c
#include "rpc/plugin_api.h"

static Json_Value* my_handler(const ExecutionContext* ctx, const Json_Value* params) {
    // Your logic here
    return create_json_response(...);
}

extern "C" int plugin_init(DineroPluginApi* api) {
    api->registerHandler("myplugin.method", my_handler, "plugin");
    return 0;
}

extern "C" void plugin_shutdown() {
    // Cleanup
}
```

### Build Command

**Linux:**
```bash
gcc -shared -fPIC -I../include myplugin.c -o myplugin.so
```

**macOS:**
```bash
clang -shared -fPIC -I../include myplugin.c -o myplugin.dylib
```

**Windows:**
```bash
cl /LD /I..\include myplugin.c /Fe:myplugin.dll
```

## Loading Plugins

### Via Command Line

```bash
dinerod --plugin-dir=/path/to/plugins
```

### Via Configuration File

Add to `dinero.conf`:
```ini
plugin-dir=/path/to/plugins
plugin-load=/path/to/specific_plugin.so
```

### At Runtime (via RPC)

```bash
dinero-cli plugin.load /path/to/plugin.so
dinero-cli plugin.list
dinero-cli plugin.unload my-plugin
```

## API Reference

See `include/rpc/plugin_api.h` for full API documentation.

### Key Functions

- `registerHandler(method, handler, category)` - Register RPC method
- `log_info/warn/error(level, message)` - Logging
- `get_version()` - Daemon version
- `get_network()` - Network type (mainnet/testnet/regtest)
- `get_block_height()` - Current blockchain height

## Example Plugin

See `example_plugin.cpp` for a complete working example that implements:
- `myplugin.hello` - Greet someone
- `myplugin.status` - Plugin status
- `myplugin.echo` - Echo parameters

## Security

⚠️ **Plugins run with full daemon privileges!**

- Only load plugins from trusted sources
- Review plugin source code before loading
- Plugins can access blockchain data, wallet, and network

## Best Practices

1. **Namespace your methods** - Use `pluginname.method` pattern
2. **Handle errors gracefully** - Return error JSON instead of crashing
3. **Document your plugin** - Implement `plugin_metadata()`
4. **Clean up resources** - Use `plugin_shutdown()`
5. **Version your plugin** - Include version in metadata

## Advanced: C++ Helper Macros

```cpp
#include "rpc/plugin_api.h"

DINERO_PLUGIN_BEGIN(myplugin)

DINERO_PLUGIN_METHOD(myplugin.hello, "plugin") {
    din::Json result;
    result["message"] = "Hello from C++ plugin!";
    return result;
}

DINERO_PLUGIN_INIT()
    register_myplugin_hello();
DINERO_PLUGIN_END()
```

## Troubleshooting

**Plugin won't load:**
- Check API version compatibility
- Verify shared library dependencies
- Check daemon logs for errors

**Method not callable:**
- Ensure plugin_init() returned 0
- Verify method was registered
- Check method name spelling

**Crashes:**
- Verify ExecutionContext pointer is valid
- Don't hold references after handler returns
- Use daemon's JSON helpers for memory safety
