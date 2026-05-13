// SPDX-License-Identifier: MIT
// Phase W.4.4: Fee Bump RPC Handler Implementations

#include "rpc/fee_bump_rpc_handlers.h"
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include "mining/tx_inclusion_analyzer.h"
#include "wallet/rbf_cpfp_detector.h"
#include "wallet/fee_bump_engine.h"
#include "daemon/execution_context.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "mempool/mempool.h"
#include "wallet/wallet_manager.h"
#include "mining/block_assembler.h"
#include "rpc/ergonomics_rpc_handlers.h"  // For NodeHealth
#include "common/logger.h"
#include <stdexcept>
#include <sstream>

using namespace dinero;

extern RpcRegistry g_rpcRegistry;

// ============================================================================
// Global State Management for W.4 Components
// ============================================================================

namespace {

/**
 * @brief Get global TxInclusionAnalyzer instance (singleton)
 */
TxInclusionAnalyzer& GetTxInclusionAnalyzer() {
    static TxInclusionAnalyzer instance;
    return instance;
}

/**
 * @brief Get global RbfCpfpDetector instance (singleton)
 */
RbfCpfpDetector& GetRbfCpfpDetector() {
    static RbfCpfpDetector instance;
    return instance;
}

/**
 * @brief Get global FeeBumpEngine instance (singleton)
 */
FeeBumpEngine& GetFeeBumpEngine() {
    static FeeBumpEngine instance;
    return instance;
}

} // anonymous namespace

// ============================================================================
// Helper Functions: JSON Serialization
// ============================================================================

namespace {

struct FeeBumpRuntimeDeps {
    Mempool* mempool{nullptr};
    WalletManager* wallet{nullptr};
    BlockAssembler* block_assembler{nullptr};
};

FeeBumpRuntimeDeps ResolveFeeBumpRuntimeDeps() {
    FeeBumpRuntimeDeps deps;

    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx) {
        return deps;
    }

    if (daemon_ctx->mempool) {
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(daemon_ctx->mempool);
        if (mempool_service && mempool_service->isInitialized()) {
            deps.mempool = &mempool_service->mempool();
        }
    }

    if (daemon_ctx->wallet) {
        auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(daemon_ctx->wallet);
        if (wallet_service) {
            deps.wallet = &wallet_service->get();
        }
    }

    deps.block_assembler = daemon_ctx->block_assembler.get();
    return deps;
}

/**
 * @brief Serialize TxInclusionStatus to JSON
 */
Json::Value SerializeTxInclusionStatus(const TxInclusionStatus& status) {
    Json::Value result;

    result["txid"] = status.txid.ToString();
    result["state"] = InclusionStateToString(status.state);
    result["primary_reason"] = InclusionReasonToString(status.primary_reason);
    result["estimated_inclusion_prob"] = status.estimated_inclusion_prob;
    result["effective_feerate"] = Json::Value::UInt64(status.effective_feerate);
    result["cutoff_feerate"] = Json::Value::UInt64(status.cutoff_feerate);
    result["rbf_available"] = status.rbf_available;
    result["cpfp_available"] = status.cpfp_available;

    if (status.suggested_bump_feerate.has_value()) {
        result["suggested_bump_feerate"] = Json::Value::UInt64(*status.suggested_bump_feerate);
    }

    result["explanation"] = status.explanation;
    result["timestamp_ms"] = Json::Value::UInt64(status.timestamp_ms);

    return result;
}

/**
 * @brief Serialize RbfRecommendation to JSON
 */
Json::Value SerializeRbfRecommendation(const RbfRecommendation& rbf) {
    Json::Value result;

    result["viable"] = rbf.viable;
    result["original_fee"] = Json::Value::UInt64(rbf.original_fee);
    result["min_replacement_fee"] = Json::Value::UInt64(rbf.min_replacement_fee);
    result["recommended_fee"] = Json::Value::UInt64(rbf.recommended_fee);
    result["recommended_feerate"] = Json::Value::UInt64(rbf.recommended_feerate);
    result["additional_cost"] = Json::Value::UInt64(rbf.additional_cost);
    result["explanation"] = rbf.explanation;

    return result;
}

/**
 * @brief Serialize CpfpRecommendation to JSON
 */
Json::Value SerializeCpfpRecommendation(const CpfpRecommendation& cpfp) {
    Json::Value result;

    result["viable"] = cpfp.viable;
    result["parent_txid"] = cpfp.parent_txid.ToString();
    result["output_index"] = cpfp.output_index;
    result["output_amount"] = Json::Value::UInt64(cpfp.output_amount);
    result["parent_fee"] = Json::Value::UInt64(cpfp.parent_fee);
    result["parent_feerate"] = Json::Value::UInt64(cpfp.parent_feerate);
    result["recommended_child_fee"] = Json::Value::UInt64(cpfp.recommended_child_fee);
    result["recommended_child_feerate"] = Json::Value::UInt64(cpfp.recommended_child_feerate);
    result["package_feerate"] = Json::Value::UInt64(cpfp.package_feerate);
    result["total_cost"] = Json::Value::UInt64(cpfp.total_cost);
    result["explanation"] = cpfp.explanation;

    return result;
}

/**
 * @brief Serialize FeeBumpRecommendation to JSON
 */
Json::Value SerializeFeeBumpRecommendation(const FeeBumpRecommendation& recommendation) {
    Json::Value result;

    result["txid"] = recommendation.txid.ToString();
    result["strategy"] = BumpStrategyToString(recommendation.strategy);
    result["rationale"] = recommendation.rationale;
    result["current_feerate"] = Json::Value::UInt64(recommendation.current_feerate);
    result["target_feerate"] = Json::Value::UInt64(recommendation.target_feerate);
    result["mempool_min_feerate"] = Json::Value::UInt64(recommendation.mempool_min_feerate);
    result["estimated_blocks_current"] = recommendation.estimated_blocks_current;
    result["estimated_blocks_target"] = recommendation.estimated_blocks_target;

    // Add RBF details if available
    if (recommendation.rbf.has_value()) {
        result["rbf"] = SerializeRbfRecommendation(*recommendation.rbf);
    }

    // Add CPFP details if available
    if (recommendation.cpfp.has_value()) {
        result["cpfp"] = SerializeCpfpRecommendation(*recommendation.cpfp);
    }

    // Add warnings array
    Json::Value warnings(Json::arrayValue);
    for (const auto& warning : recommendation.warnings) {
        warnings.append(warning);
    }
    result["warnings"] = warnings;

    result["timestamp_ms"] = Json::Value::UInt64(recommendation.timestamp_ms);

    return result;
}

} // anonymous namespace

// ============================================================================
// RPC Handler: wallet.gettxinclusion
// ============================================================================

Json::Value dinero::wallet_gettxinclusion(
    const Json::Value& params
) {
    dinero::g_logger.debug("RPC: wallet.gettxinclusion called");

    // Validate parameters
    if (params.size() < 1) {
        throw std::runtime_error("Missing required parameter: txid");
    }

    if (!params[0].isString()) {
        throw std::runtime_error("Invalid parameter: txid must be a string");
    }

    // Parse txid
    std::string txid_str = params[0].asString();
    uint256 txid = uint256::FromHexUnsafe(txid_str);

    // Resolve live runtime dependencies from daemon context.
    FeeBumpRuntimeDeps deps = ResolveFeeBumpRuntimeDeps();
    Mempool* mempool = deps.mempool;
    BlockAssembler* block_assembler = deps.block_assembler;
    NodeHealth* node_health = nullptr;
    WalletManager* wallet = deps.wallet;

    // Analyze transaction inclusion
    TxInclusionAnalyzer& analyzer = GetTxInclusionAnalyzer();
    TxInclusionStatus status = analyzer.Analyze(
        txid,
        mempool,
        block_assembler,
        node_health
    );

    // Also check RBF/CPFP capabilities
    RbfCpfpDetector& detector = GetRbfCpfpDetector();

    RbfCapability rbf_cap = detector.CheckRbfCapability(txid, mempool);
    CpfpCapability cpfp_cap = detector.CheckCpfpCapability(txid, wallet, mempool);

    status.rbf_available = rbf_cap.can_be_replaced;
    status.cpfp_available = cpfp_cap.viable;

    // Serialize to JSON
    return SerializeTxInclusionStatus(status);
}

// ============================================================================
// RPC Handler: wallet.gettxbumprecommendation
// ============================================================================

Json::Value dinero::wallet_gettxbumprecommendation(
    const Json::Value& params
) {
    dinero::g_logger.debug("RPC: wallet.gettxbumprecommendation called");

    // Validate parameters
    if (params.size() < 1) {
        throw std::runtime_error("Missing required parameter: txid");
    }

    if (!params[0].isString()) {
        throw std::runtime_error("Invalid parameter: txid must be a string");
    }

    // Parse txid
    std::string txid_str = params[0].asString();
    uint256 txid = uint256::FromHexUnsafe(txid_str);

    // Parse optional target_blocks parameter
    uint32_t target_blocks = 1;  // Default: next block
    if (params.size() >= 2) {
        if (!params[1].isNumeric()) {
            throw std::runtime_error("Invalid parameter: target_blocks must be a number");
        }
        target_blocks = params[1].asUInt();
        if (target_blocks == 0) {
            throw std::runtime_error("Invalid parameter: target_blocks must be > 0");
        }
        if (target_blocks > 1008) {  // 1 week at 10 min blocks
            throw std::runtime_error("Invalid parameter: target_blocks must be <= 1008");
        }
    }

    // Resolve live runtime dependencies from daemon context.
    FeeBumpRuntimeDeps deps = ResolveFeeBumpRuntimeDeps();
    Mempool* mempool = deps.mempool;
    WalletManager* wallet = deps.wallet;
    BlockAssembler* block_assembler = deps.block_assembler;
    NodeHealth* node_health = nullptr;

    // Step 1: Analyze transaction inclusion status
    TxInclusionAnalyzer& inclusion_analyzer = GetTxInclusionAnalyzer();
    TxInclusionStatus inclusion_status = inclusion_analyzer.Analyze(
        txid,
        mempool,
        block_assembler,
        node_health
    );

    // Step 2: Detect RBF/CPFP capabilities
    RbfCpfpDetector& detector = GetRbfCpfpDetector();
    RescueStrategy rescue_strategy = detector.DetermineRescueStrategy(
        txid,
        wallet,
        mempool
    );

    // Step 3: Generate fee bump recommendation
    FeeBumpEngine& engine = GetFeeBumpEngine();
    FeeBumpRecommendation recommendation = engine.GenerateRecommendation(
        txid,
        inclusion_status,
        rescue_strategy,
        mempool,
        wallet,
        target_blocks
    );

    // Serialize to JSON
    return SerializeFeeBumpRecommendation(recommendation);
}

// ============================================================================
// RPC Registration
// ============================================================================

void dinero::RegisterFeeBumpRpcHandlers() {
    dinero::g_logger.info("Registering Phase W.4.4 fee bump RPC handlers");

    g_rpcRegistry.registerHandler(
        "wallet.gettxinclusion",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            (void)ctx;
            return wallet_gettxinclusion(params);
        },
        RegisterMode::Overwrite,
        "fee_bump");
    g_rpcRegistry.registerAlias("gettxinclusion", "wallet.gettxinclusion");

    g_rpcRegistry.registerHandler(
        "wallet.gettxbumprecommendation",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            (void)ctx;
            return wallet_gettxbumprecommendation(params);
        },
        RegisterMode::Overwrite,
        "fee_bump");
    g_rpcRegistry.registerAlias("gettxbumprecommendation", "wallet.gettxbumprecommendation");

    dinero::g_logger.info("Phase W.4.4 RPC handlers registered");
}
