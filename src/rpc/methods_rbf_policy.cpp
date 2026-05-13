// Phase 5E: RBF (Replace-By-Fee) Policy RPC Methods
// Architecture V3: Context-aware handlers for RBF policy management

#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "common/logger.h"
#include "din_json.h"
#include <algorithm>

namespace dinero {
namespace rpc {

using din::Json;
using ::ExecutionContext;

static std::shared_ptr<MempoolService> requireMempoolService(const ExecutionContext& ctx) {
    if (!ctx.daemon || !ctx.daemon->mempool) {
        throw std::runtime_error("Mempool service not available");
    }

    auto mempool_service = std::dynamic_pointer_cast<MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        throw std::runtime_error("Failed to cast mempool service");
    }

    return mempool_service;
}

// rbf.config - Get RBF policy configuration
static Json rbf_config_impl(const ExecutionContext& ctx, const Json& params) {
    auto mempool_service = requireMempoolService(ctx);
    const auto config = mempool_service->mempool().getRBFRuntimeConfig();

    Json result = din::obj();
    result["enabled"] = config.enabled;
    result["min_relay_fee_rate"] = static_cast<Json::UInt64>(config.min_relay_fee_rate);
    result["incremental_relay_fee"] = static_cast<Json::UInt64>(config.incremental_relay_fee);
    result["max_replacement_count"] = static_cast<Json::UInt64>(config.max_replacement_count);
    result["policy"] = config.enabled ? "BIP125 opt-in RBF" : "disabled";
    result["description"] = config.enabled
        ? "Replace-By-Fee is enabled and enforced by the live mempool"
        : "Replace-By-Fee is disabled by the live mempool";

    return result;
}

// rbf.status - Get RBF policy status
static Json rbf_status_impl(const ExecutionContext& ctx, const Json& params) {
    auto mempool_service = requireMempoolService(ctx);
    const auto config = mempool_service->mempool().getRBFRuntimeConfig();

    Json result = din::obj();
    result["enabled"] = config.enabled;
    result["policy"] = config.enabled ? "BIP125 opt-in RBF" : "disabled";
    result["description"] = config.enabled
        ? "Transactions can be replaced if they signal replaceability"
        : "Transactions cannot be replaced because RBF is disabled";

    return result;
}

// rbf.estimate - Estimate required fee for replacement
static Json rbf_estimate_impl(const ExecutionContext& ctx, const Json& params) {
    auto mempool_service = requireMempoolService(ctx);
    const auto config = mempool_service->mempool().getRBFRuntimeConfig();

    // Parse parameters
    if (params.size() < 1 || !params[0].isNumeric()) {
        throw std::invalid_argument("rbf.estimate requires current_fee parameter");
    }

    uint64_t current_fee = params[0].asUInt64();
    uint64_t replacement_vsize = params.size() >= 2 && params[1].isNumeric()
        ? params[1].asUInt64()
        : 1000;
    uint64_t replaced_vsize = params.size() >= 3 && params[2].isNumeric()
        ? params[2].asUInt64()
        : replacement_vsize;

    Json result = din::obj();
    result["enabled"] = config.enabled;
    result["current_fee"] = static_cast<Json::UInt64>(current_fee);
    result["replacement_vsize"] = static_cast<Json::UInt64>(replacement_vsize);
    result["replaced_vsize"] = static_cast<Json::UInt64>(replaced_vsize);

    if (!config.enabled) {
        result["policy"] = "disabled";
        result["description"] = "RBF is disabled on this node; replacements will be rejected";
        return result;
    }

    const uint64_t total_relay_vsize = replacement_vsize + replaced_vsize;
    const uint64_t bandwidth_delta =
        (total_relay_vsize * config.incremental_relay_fee + 999) / 1000;
    const uint64_t required_additional_fee = std::max<uint64_t>(1, bandwidth_delta);
    const uint64_t required_fee = current_fee + required_additional_fee;

    result["required_fee"] = static_cast<Json::UInt64>(required_fee);
    result["required_additional_fee"] = static_cast<Json::UInt64>(required_additional_fee);
    result["incremental_relay_fee"] = static_cast<Json::UInt64>(config.incremental_relay_fee);
    result["total_relay_vsize"] = static_cast<Json::UInt64>(total_relay_vsize);
    result["description"] =
        "Estimated BIP125 Rule #4 minimum for replacing one direct conflict; actual replacements may require more fee";

    return result;
}

// Registration function called from rpc_context_wiring.cpp
void register_rbf_policy_methods() {
    // Register RBF policy RPC methods in global registry
    g_rpcRegistry.registerHandler("rbf.config",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return rbf_config_impl(ctx, params);
        },
        "rbf");

    g_rpcRegistry.registerHandler("rbf.status",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return rbf_status_impl(ctx, params);
        },
        "rbf");

    g_rpcRegistry.registerHandler("rbf.estimate",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return rbf_estimate_impl(ctx, params);
        },
        "rbf");
}

} // namespace rpc
} // namespace dinero
