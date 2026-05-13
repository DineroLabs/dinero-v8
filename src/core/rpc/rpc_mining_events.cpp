#include "daemon/mining_events.h"
#include "compat/jsoncpp_compat.h"
#include <algorithm>

// Forward declaration for RpcContext - we'll need to add MiningEventBus* to it
struct RpcContext {
    // ... existing members ...
    MiningEventBus* events = nullptr;
};

Json::Value rpc_mining_events(RpcContext& ctx, const Json::Value& p) {
    if (!ctx.events) {
        Json::Value error;
        error["code"] = -32603;
        error["message"] = "Mining events not available";
        return error;
    }
    
    const int64_t since = p.isMember("since") ? p["since"].asInt64() : 0;
    const int max = p.isMember("max") ? std::max(1, p["max"].asInt()) : 200;
    return ctx.events->snapshot(since, max);
}
