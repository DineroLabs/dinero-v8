/**
 * CT Fee Configuration RPC Methods - Phase 3 (CT Fee Market Tuning)
 *
 * Provides RPC commands for configuring Confidential Transaction fee policies:
 * - ct.setminfee         : Set minimum CT fee rate (una/vB)
 * - ct.setweightmultiplier: Set CT weight multiplier
 * - ct.setmaxperblock    : Set max CT transactions per block
 * - ct.getfeeconfig      : Get current CT fee configuration
 * - ct.estimatefee       : Estimate fee for CT transaction
 *
 * Design:
 * - Context-aware (accesses Mempool via ctx.daemon->mempool)
 * - Thread-safe (Mempool handles locking internally)
 * - Validates all parameters before applying
 */

#include "rpc/ct_fee_rpc.h"
#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "mempool/fee_estimator.h"
#include "mining/ct_selection_policy.h"
#include <cmath>

namespace {

// ═══════════════════════════════════════════════════════════════
// CT FEE CONFIGURATION RPC HANDLERS
// ═══════════════════════════════════════════════════════════════

/**
 * ct.setminfee - Set minimum CT fee rate
 *
 * Parameters:
 * - fee_rate (required): Minimum fee rate in una/vB (1-1000)
 *
 * Example:
 * > ct.setminfee 3
 * { "status": "updated", "ct_min_fee_rate": 3 }
 */
din::Json rpc_ct_setminfee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Validate mempool service
    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    // Validate parameters
    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing fee_rate parameter. Usage: ct.setminfee <fee_rate>";
        return result;
    }

    uint64_t fee_rate = params[0].asUInt64();
    if (fee_rate < 1 || fee_rate > 1000) {
        result["error"] = "Fee rate must be 1-1000 una/vB";
        return result;
    }

    // Update config
    auto& config = mempool_service->mempool().GetCTConfig();
    config.ct_min_fee_rate = fee_rate;

    result["status"] = "updated";
    result["ct_min_fee_rate"] = fee_rate;
    return result;
}

/**
 * ct.setweightmultiplier - Set CT weight multiplier
 *
 * Parameters:
 * - multiplier (required): Weight multiplier (1.0-10.0)
 *
 * Example:
 * > ct.setweightmultiplier 1.8
 * { "status": "updated", "ct_weight_multiplier": 1.8 }
 */
din::Json rpc_ct_setweightmultiplier(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing multiplier parameter. Usage: ct.setweightmultiplier <multiplier>";
        return result;
    }

    double multiplier = params[0].asDouble();
    if (multiplier < 1.0 || multiplier > 10.0) {
        result["error"] = "Weight multiplier must be 1.0-10.0";
        return result;
    }

    auto& config = mempool_service->mempool().GetCTConfig();
    config.ct_weight_multiplier = multiplier;

    result["status"] = "updated";
    result["ct_weight_multiplier"] = multiplier;
    return result;
}

/**
 * ct.setmaxperblock - Set max CT transactions per block
 *
 * Parameters:
 * - max_count (required): Maximum CT transactions per block (1-500)
 *
 * Example:
 * > ct.setmaxperblock 75
 * { "status": "updated", "ct_max_per_block": 75 }
 */
din::Json rpc_ct_setmaxperblock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing max_count parameter. Usage: ct.setmaxperblock <max_count>";
        return result;
    }

    uint32_t max_count = params[0].asUInt();
    if (max_count < 1 || max_count > 500) {
        result["error"] = "Max per block must be 1-500";
        return result;
    }

    auto& config = mempool_service->mempool().GetCTConfig();
    config.max_ct_per_block = max_count;

    result["status"] = "updated";
    result["max_ct_per_block"] = max_count;
    return result;
}

/**
 * ct.getfeeconfig - Get current CT fee configuration
 *
 * Returns all CT fee policy parameters.
 *
 * Example:
 * > ct.getfeeconfig
 * {
 *   "ct_min_fee_rate": 2,
 *   "ct_weight_multiplier": 1.5,
 *   "ct_max_per_block": 50,
 *   "ct_proof_weight_factor": 4
 * }
 */
din::Json rpc_ct_getfeeconfig(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    const auto& config = mempool_service->mempool().GetCTConfig();

    result["ct_min_fee_rate"] = config.ct_min_fee_rate;
    result["ct_weight_multiplier"] = config.ct_weight_multiplier;
    result["max_ct_per_block"] = static_cast<uint64_t>(config.max_ct_per_block);
    result["ct_proof_weight_factor"] = config.ct_proof_weight_factor;

    return result;
}

/**
 * ct.estimatefee - Estimate fee for CT transaction
 *
 * Calculates the recommended fee rate for a CT transaction based on
 * current mempool conditions and proof size overhead.
 *
 * Parameters:
 * - proof_bytes (optional, default=5000): Expected total proof size in bytes
 * - target_blocks (optional, default=6): Confirmation target in blocks
 *
 * Example:
 * > ct.estimatefee 5000 6
 * {
 *   "base_fee_rate": 10,
 *   "ct_adjusted_rate": 15,
 *   "ct_multiplier": 1.5,
 *   "proof_bytes": 5000,
 *   "target_blocks": 6
 * }
 */
din::Json rpc_ct_estimatefee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Failed to cast mempool service";
        return result;
    }

    // Parse optional parameters
    size_t proof_bytes = 5000;  // Default: ~7 outputs worth of proofs
    int target_blocks = 6;      // Default: 6 block confirmation target

    if (params.isArray()) {
        if (params.size() >= 1) {
            proof_bytes = params[0].asUInt64();
        }
        if (params.size() >= 2) {
            target_blocks = params[1].asInt();
            if (target_blocks < 1 || target_blocks > 1008) {
                result["error"] = "Target blocks must be 1-1008";
                return result;
            }
        }
    }

    const auto& config = mempool_service->mempool().GetCTConfig();

    // Get base fee estimate from the mempool fee estimator.
    auto estimated_opt =
        mempool_service->mempool().getFeeEstimator().estimateFee(static_cast<uint32_t>(target_blocks));
    double estimated_fee_rate = estimated_opt.value_or(1.0);  // una/vB
    if (!std::isfinite(estimated_fee_rate) || estimated_fee_rate <= 0.0) {
        estimated_fee_rate = 1.0;
    }
    uint64_t base_fee_rate = static_cast<uint64_t>(std::ceil(estimated_fee_rate));  // una/vB

    // Calculate CT-adjusted fee rate
    // CT transactions need higher fees due to proof verification overhead
    uint64_t ct_adjusted_rate = static_cast<uint64_t>(
        base_fee_rate * config.ct_weight_multiplier
    );

    // Ensure minimum CT fee rate is met
    if (ct_adjusted_rate < config.ct_min_fee_rate) {
        ct_adjusted_rate = config.ct_min_fee_rate;
    }

    result["base_fee_rate"] = base_fee_rate;
    result["ct_adjusted_rate"] = ct_adjusted_rate;
    result["ct_multiplier"] = config.ct_weight_multiplier;
    result["proof_bytes"] = static_cast<uint64_t>(proof_bytes);
    result["target_blocks"] = target_blocks;

    return result;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

void register_ct_fee_rpc_methods(RpcRegistry& registry) {
    // ct.setminfee
    RpcMethodMeta setminfee_meta;
    setminfee_meta.name = "setminfee";
    setminfee_meta.ns = "ct";
    setminfee_meta.description = "Set minimum CT fee rate (una/vB)";
    setminfee_meta.result.type = "object";
    setminfee_meta.result.desc = "{ status, ct_min_fee_rate }";
    registry.registerHandler("ct.setminfee", rpc_ct_setminfee, setminfee_meta, "Phase 3");

    // ct.setweightmultiplier
    RpcMethodMeta setweight_meta;
    setweight_meta.name = "setweightmultiplier";
    setweight_meta.ns = "ct";
    setweight_meta.description = "Set CT weight multiplier";
    setweight_meta.result.type = "object";
    setweight_meta.result.desc = "{ status, ct_weight_multiplier }";
    registry.registerHandler("ct.setweightmultiplier", rpc_ct_setweightmultiplier, setweight_meta, "Phase 3");

    // ct.setmaxperblock
    RpcMethodMeta setmax_meta;
    setmax_meta.name = "setmaxperblock";
    setmax_meta.ns = "ct";
    setmax_meta.description = "Set max CT transactions per block";
    setmax_meta.result.type = "object";
    setmax_meta.result.desc = "{ status, max_ct_per_block }";
    registry.registerHandler("ct.setmaxperblock", rpc_ct_setmaxperblock, setmax_meta, "Phase 3");

    // ct.getfeeconfig
    RpcMethodMeta getconfig_meta;
    getconfig_meta.name = "getfeeconfig";
    getconfig_meta.ns = "ct";
    getconfig_meta.description = "Get current CT fee configuration";
    getconfig_meta.result.type = "object";
    getconfig_meta.result.desc = "{ ct_min_fee_rate, ct_weight_multiplier, max_ct_per_block, ct_proof_weight_factor }";
    registry.registerHandler("ct.getfeeconfig", rpc_ct_getfeeconfig, getconfig_meta, "Phase 3");

    // ct.estimatefee
    RpcMethodMeta estimate_meta;
    estimate_meta.name = "estimatefee";
    estimate_meta.ns = "ct";
    estimate_meta.description = "Estimate fee for CT transaction";
    estimate_meta.result.type = "object";
    estimate_meta.result.desc = "{ base_fee_rate, ct_adjusted_rate, ct_multiplier, proof_bytes, target_blocks }";
    registry.registerHandler("ct.estimatefee", rpc_ct_estimatefee, estimate_meta, "Phase 3");
}
