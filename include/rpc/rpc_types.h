#pragma once

#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
#include <functional>
#include <string>

namespace rpc {
    // Canonical RPC types - everything uses Json::Value
    using Params = Json::Value;   // object or array per JSON-RPC
    using Id     = Json::Value;   // number | string | null
    using Result = Json::Value;   // structured result

    // Unified handler signature - the one true type
    using Handler = std::function<Result(const Params&, const Id&)>;
    
    // Legacy string handler type for adaptation
    using StringHandler = std::function<std::string(const Params&, const Id&)>;
    
    // Simple string handler (old style with just params)
    using SimpleStringHandler = std::function<std::string(const std::string&)>;
    
    // Simple Json handler (params only)
    using SimpleJsonHandler = std::function<Json::Value(const Json::Value&)>;
}
