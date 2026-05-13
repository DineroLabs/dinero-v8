#include <string>
#include "compat/jsoncpp_compat.h"
using json = Json::Value;

namespace dinero {

// Stub implementation of daemon_rpc_dispatch for embedded node
// This is called by RPCServer but we don't need the full dispatch functionality
json daemon_rpc_dispatch(
    const std::string& path,
    const std::string& method,
    const json& params,
    const json& id
) {
    // Return a simple error for unsupported methods
    json result;
    result["error"] = {
        {"code", -32601},
        {"message", "Method not supported in embedded mode"}
    };
    result["id"] = id;
    return result;
}

} // namespace dinero
