// SPDX-License-Identifier: MIT
// Dinero - GPU Mining RPC Handlers
//
// Week 9 - GPU Mining Integration
// Implements three RPC commands for GPU mining control:
// - mining.gpustatus - Enumerate detected GPUs
// - mining.allowgpu  - Enable/disable GPU mining at specific height
// - mining.gpuinfo   - Get GPU mining statistics

#include "rpc/rpc_registry.h"
#include "compat/jsoncpp_compat.h"
#include "common/logger.h"
#include "daemon/services/mining_service.h"  // Phase C: MiningManager v2 access

// GPU mining headers
#ifdef ENABLE_GPU_MINING
#include "mining/gpu/gpu_device_manager.h"
#include "mining/gpu/compute_backend.h"
#endif

// Core dependencies
// #include "mining/mining_manager.h"  // Phase C: Removed - use MiningManager v2 through MiningService
#include "consensus/consensus.hpp"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"

// GPU mining governance globals (simpler than accessing through consensus interface)
static uint32_t s_gpuMiningActivationHeight = 0;
static bool s_allowGPUMining = false;

// Forward declaration for global RPC registry
extern RpcRegistry g_rpcRegistry;

namespace din {
namespace rpc {

// ============================================================================
// RPC Command 1: mining.gpustatus
// ============================================================================
// Purpose: Enumerate all detected GPUs and show their availability
// Returns: List of GPU devices with specs + governance parameters
// ============================================================================

din::Json rpc_mining_gpustatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response(::Json::objectValue);

    // Return GPU mining governance parameters
    response["gpu_mining_enabled"] = s_allowGPUMining;
    response["activation_height"] = static_cast<uint64_t>(s_gpuMiningActivationHeight);

#ifdef ENABLE_GPU_MINING
    try {
        // Get GPU device manager
        dinero::gpu::GPUDeviceManager gpu_manager;
        auto devices = gpu_manager.enumerateAllDevices();

        // Build device array
        din::Json device_array(::Json::arrayValue);
        for (const auto& dev : devices) {
            din::Json device_obj(::Json::objectValue);
            device_obj["id"] = dev.device_id;
            device_obj["name"] = dev.name;
            device_obj["vendor"] = dev.vendor;

            // Backend type string
            std::string backend_str;
            switch (dev.backend) {
                case dinero::gpu::BackendType::OPENCL:
                    backend_str = "OpenCL";
                    break;
                case dinero::gpu::BackendType::CUDA:
                    backend_str = "CUDA";
                    break;
                case dinero::gpu::BackendType::METAL:
                    backend_str = "Metal";
                    break;
                default:
                    backend_str = "None";
                    break;
            }
            device_obj["backend"] = backend_str;

            device_obj["compute_units"] = dev.compute_units;
            device_obj["clock_mhz"] = dev.max_clock_mhz;
            device_obj["memory_mb"] = static_cast<uint64_t>(dev.global_memory_mb);
            device_obj["available"] = dev.available;

            device_array.append(device_obj);
        }

        response["devices"] = device_array;
    } catch (const std::exception& e) {
        dinero::g_logger.error(std::string("[GPU RPC] mining.gpustatus error: ") + e.what());
        response["devices"] = din::Json(::Json::arrayValue);
    }
#else
    // GPU mining not compiled in
    response["devices"] = din::Json(::Json::arrayValue);
    response["note"] = "GPU mining support not compiled (ENABLE_GPU_MINING=OFF)";
#endif

    return response;
}


// ============================================================================
// RPC Command 2: mining.allowgpu
// ============================================================================
// Purpose: Set GPU mining activation height and enable/disable GPU mining
// Syntax:  mining.allowgpu <height>
//          height = 0: Disable GPU mining
//          height > 0: Enable GPU mining at that block height
// ============================================================================

din::Json rpc_mining_allowgpu(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response(::Json::objectValue);

    // Validate daemon context
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        response["error"]["code"] = -1;
        response["error"]["message"] = "Daemon context not available";
        return response;
    }

    // Validate parameters
    if (!params.isArray() || params.size() < 1) {
        response["error"]["code"] = -1;
        response["error"]["message"] = "Missing height parameter. Usage: mining.allowgpu <height>";
        return response;
    }

    // Parse activation height
    uint32_t activation_height = 0;
    if (params[0].isUInt() || params[0].isInt()) {
        activation_height = params[0].asUInt();
    } else if (params[0].isString()) {
        try {
            activation_height = std::stoul(params[0].asString());
        } catch (...) {
            response["error"]["code"] = -1;
            response["error"]["message"] = "Invalid height parameter (must be a number)";
            return response;
        }
    } else {
        response["error"]["code"] = -1;
        response["error"]["message"] = "Invalid height parameter type";
        return response;
    }

    // Set activation height in static storage
    s_gpuMiningActivationHeight = activation_height;

    // Get current blockchain height from chainstate service
    uint32_t current_height = 0;
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (chainstate) {
        current_height = chainstate->getBlockHeight();
    }

    // Update runtime flag based on height
    if (activation_height > 0 && current_height >= activation_height) {
        s_allowGPUMining = true;
    } else {
        s_allowGPUMining = false;
    }

    // Build response
    response["activation_height"] = activation_height;
    response["current_height"] = current_height;
    response["gpu_mining_enabled"] = s_allowGPUMining;

    // Status message
    if (activation_height > current_height) {
        std::string msg = "GPU mining will be enabled at block " + std::to_string(activation_height);
        response["message"] = msg;
        dinero::g_logger.info("[GPU RPC] " + msg);
    } else if (activation_height > 0) {
        response["message"] = "GPU mining is now enabled";
        dinero::g_logger.info("[GPU RPC] GPU mining enabled");
    } else {
        response["message"] = "GPU mining is now disabled";
        dinero::g_logger.info("[GPU RPC] GPU mining disabled");
    }

    return response;
}


// ============================================================================
// RPC Command 3: mining.gpuinfo
// ============================================================================
// Purpose: Get GPU mining statistics (hashrate, active GPUs, etc.)
// Returns: Current GPU mining status and statistics from MiningManager
// ============================================================================

din::Json rpc_mining_gpuinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json response(::Json::objectValue);

    // Phase C: Get MiningManager v2 through MiningService
    auto* mining_svc = ctx.daemon->mining.get();
    if (!mining_svc) {
        throw std::runtime_error("Mining service not available");
    }

    // Get mining stats from MiningManager v2 (access atomics directly, don't copy)
    const auto& stats = mining_svc->getMiningManager().getStats();

    // Convert stats to old MiningInfo format for compatibility
    struct {
        bool gpu_mining_enabled = false;
        bool gpu_available = false;
        double gpu_hashrate = 0.0;
        int gpu_device_count = 0;
        std::string gpu_device_name = "";
        std::string gpu_backend = "None";
        double hashrate = 0.0;
    } mining_info;

    mining_info.hashrate = stats.current_hashrate.load();
    // MiningStats v2 is CPU-only in this build; GPU fields remain intentionally zeroed.

    // Build response
    response["gpu_mining_active"] = mining_info.gpu_mining_enabled;
    response["gpu_available"] = mining_info.gpu_available;
    response["gpu_hashrate"] = mining_info.gpu_hashrate;
    response["active_gpus"] = mining_info.gpu_device_count;

    // GPU device information
    din::Json gpu_stats(::Json::arrayValue);

    if (mining_info.gpu_mining_enabled && mining_info.gpu_device_count > 0) {
        din::Json gpu_obj(::Json::objectValue);
        gpu_obj["device_id"] = 0;  // Currently single GPU support
        gpu_obj["name"] = mining_info.gpu_device_name;
        gpu_obj["hashrate"] = mining_info.gpu_hashrate;
        gpu_obj["backend"] = mining_info.gpu_backend;
        gpu_stats.append(gpu_obj);
    }

    response["gpus"] = gpu_stats;

    // Combined CPU + GPU hashrate
    response["total_hashrate"] = mining_info.hashrate;
    response["cpu_hashrate"] = mining_info.hashrate - mining_info.gpu_hashrate;

    return response;
}


// ============================================================================
// RPC Registration
// ============================================================================
// Called from rpc_startup.cpp to register all GPU mining RPC commands
// ============================================================================

void registerGPUMiningRPCHandlers() {
    dinero::g_logger.info("[GPU RPC] Registering GPU mining RPC commands...");

    // Register mining.gpustatus
    g_rpcRegistry.registerHandler("mining.gpustatus",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_mining_gpustatus(ctx, params);
        },
        "gpu_mining");

    // Register mining.allowgpu
    g_rpcRegistry.registerHandler("mining.allowgpu",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_mining_allowgpu(ctx, params);
        },
        "gpu_mining");

    // Register mining.gpuinfo
    g_rpcRegistry.registerHandler("mining.gpuinfo",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_mining_gpuinfo(ctx, params);
        },
        "gpu_mining");

    dinero::g_logger.info("[GPU RPC] GPU mining RPC commands registered successfully");
}

} // namespace rpc
} // namespace din
