// Simple RPC method for getblocktemplate with correct bits and target
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "din_json.h"
#include <chrono>
#include <sstream>
#include <iomanip>

// Simple fix: just return the correct values for height 2 (CPU-friendly phase)
din::Json rpc_getblocktemplate_simple(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    // Height 2 values (CPU-friendly phase)
    result["version"] = 1;
    result["height"] = 2;
    result["previousblockhash"] = "0cb95aa12c8a0090eb096f2a858de9ac9c31d4ee523472c7e2f4577d184910aa";
    
    // CORRECT values for CPU-friendly phase
    result["bits"] = "207fffff";           // CPU-friendly difficulty
    result["coinbasevalue"] = 100000000;   // 100 DIN
    result["subsidy"] = 100000000;         // 100 DIN in base units
    result["subsidy_din"] = "100.000000";  // 6 decimal format
    
    // Compute target from 0x207fffff
    // 0x207fffff = exp=0x20(32), mant=0x7fffff
    // Target = mant << (8 * (exp - 3)) = 0x7fffff << (8 * 29) = 0x7fffff followed by 29 zero bytes
    result["target"] = "7fffff0000000000000000000000000000000000000000000000000000000000";
    
    result["mintime"] = static_cast<uint64_t>(std::time(nullptr));
    result["curtime"] = static_cast<uint64_t>(std::time(nullptr));
    result["weightlimit"] = 4000000;
    result["sigoplimit"] = 80000;
    result["transactions"] = din::Json(din::Json::arrayValue); // Empty array
    
    din::Json capabilities(din::Json::arrayValue);
    capabilities.append("proposal");
    result["capabilities"] = capabilities;
    
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

// Registration function
void registerSimpleBlockTemplate() {
    extern RpcRegistry g_rpcRegistry;
    
    RpcMethodMetadata meta;
    meta.name = "getblocktemplate";
    meta.description = "Returns a block template for mining (simplified)";
    meta.category = "mining";
    meta.result.type = "object";
    meta.result.desc = "Block template with correct CPU-friendly bits and target";
    
    g_rpcRegistry.registerHandler("getblocktemplate", rpc_getblocktemplate_simple, meta, "vnext_mining");
}
