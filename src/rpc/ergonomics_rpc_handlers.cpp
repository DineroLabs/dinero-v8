// SPDX-License-Identifier: MIT
// Phase W.3: RPC Ergonomics - High-level status aggregation

#include "rpc/ergonomics_rpc_handlers.h"
#include "rpc/wallet_sync_aggregator.h"
#include "rpc/rpc_registry.h"
#include "wallet/wallet_sync_status.h"
#include "daemon/mempool.h"
#include "daemon/services/p2p_service.h"  // For GetPeerCount()
#include "daemon/daemon_context.h"        // For DaemonContext access
#include "mining/transaction_scorer.h"    // FeeHistogram for congestion
#include "common/logger.h"
#include "json/json.h"
#include <chrono>

namespace dinero {

// ============================================================================
// NodeHealthGrade conversion
// ============================================================================

std::string NodeHealthGradeToString(NodeHealthGrade grade) {
    switch (grade) {
        case NodeHealthGrade::GREEN:  return "green";
        case NodeHealthGrade::YELLOW: return "yellow";
        case NodeHealthGrade::RED:    return "red";
        default:                       return "unknown";
    }
}

// ============================================================================
// Build NodeHealth from system state
// ============================================================================

NodeHealth BuildNodeHealth(const ExecutionContext& ctx) {
    NodeHealth health;
    health.timestamp_ms = GetCurrentTimeMs();

    // Initialize overall grade as GREEN, downgrade as issues are found
    health.overall_grade = NodeHealthGrade::GREEN;

    // ========================================================================
    // Subsystem 1: Blockchain Sync
    // ========================================================================
    {
        SubsystemHealth sync_health;
        sync_health.name = "sync";

        try {
            WalletSyncStatus sync_status = BuildWalletSyncStatusFromContext(ctx);

            // Determine sync health based on phase and progress
            if (sync_status.phase == SyncPhase::STEADY_STATE &&
                sync_status.overall_progress >= 0.99) {
                sync_health.grade = NodeHealthGrade::GREEN;
                sync_health.status = "Fully synced";
            } else if (sync_status.phase == SyncPhase::CATCHING_UP) {
                sync_health.grade = NodeHealthGrade::YELLOW;
                sync_health.status = "Catching up (" +
                    std::to_string(static_cast<int>(sync_status.overall_progress * 100)) + "%)";
                sync_health.warnings.push_back("Node is still synchronizing with network");
                health.overall_grade = NodeHealthGrade::YELLOW;
            } else if (sync_status.phase == SyncPhase::IBD) {
                sync_health.grade = NodeHealthGrade::RED;
                sync_health.status = "Initial block download (" +
                    std::to_string(static_cast<int>(sync_status.overall_progress * 100)) + "%)";
                sync_health.warnings.push_back("Node is far behind, not ready for production use");
                health.overall_grade = NodeHealthGrade::RED;
            } else {
                sync_health.grade = NodeHealthGrade::YELLOW;
                sync_health.status = "Syncing (" +
                    std::to_string(static_cast<int>(sync_status.overall_progress * 100)) + "%)";
            }

            // Check for reorg
            if (sync_status.is_reorg_in_progress) {
                sync_health.warnings.push_back("Active reorg in progress (depth: " +
                    std::to_string(sync_status.last_reorg_depth) + ")");
                if (health.overall_grade == NodeHealthGrade::GREEN) {
                    health.overall_grade = NodeHealthGrade::YELLOW;
                }
            }

        } catch (const std::exception& e) {
            sync_health.grade = NodeHealthGrade::RED;
            sync_health.status = "Error checking sync status";
            sync_health.warnings.push_back(std::string("Failed to query sync: ") + e.what());
            health.overall_grade = NodeHealthGrade::RED;
        }

        health.subsystems.push_back(sync_health);
    }

    // ========================================================================
    // Subsystem 2: Network Connectivity
    // ========================================================================
    {
        SubsystemHealth network_health;
        network_health.name = "network";

        // Get actual peer count from P2P service
        int peer_count = 0;
        if (ctx.daemon && ctx.daemon->p2p) {
            peer_count = static_cast<int>(ctx.daemon->p2p->GetPeerCount());
        }

        if (peer_count >= 8) {
            network_health.grade = NodeHealthGrade::GREEN;
            network_health.status = std::to_string(peer_count) + " peers connected";
        } else if (peer_count >= 3) {
            network_health.grade = NodeHealthGrade::YELLOW;
            network_health.status = std::to_string(peer_count) + " peers connected";
            network_health.warnings.push_back("Low peer count, recommend 8+ peers");
            if (health.overall_grade == NodeHealthGrade::GREEN) {
                health.overall_grade = NodeHealthGrade::YELLOW;
            }
        } else {
            network_health.grade = NodeHealthGrade::RED;
            network_health.status = std::to_string(peer_count) + " peers connected";
            network_health.warnings.push_back("Critically low peer count, network isolation risk");
            health.overall_grade = NodeHealthGrade::RED;
        }

        health.subsystems.push_back(network_health);
    }

    // ========================================================================
    // Subsystem 3: Wallet Status
    // ========================================================================
    {
        SubsystemHealth wallet_health;
        wallet_health.name = "wallet";

        if (ctx.wallet_manager != nullptr) {
            wallet_health.grade = NodeHealthGrade::GREEN;
            wallet_health.status = "Wallet loaded and operational";

            // Check if wallet is synced
            try {
                WalletSyncStatus sync_status = BuildWalletSyncStatusFromContext(ctx);
                if (sync_status.wallet_scan_height < sync_status.chain_height) {
                    wallet_health.grade = NodeHealthGrade::YELLOW;
                    wallet_health.status = "Wallet scanning blockchain";
                    wallet_health.warnings.push_back("Wallet scan in progress");
                    if (health.overall_grade == NodeHealthGrade::GREEN) {
                        health.overall_grade = NodeHealthGrade::YELLOW;
                    }
                }
            } catch (...) {
                // Ignore sync check errors for wallet health
            }
        } else {
            wallet_health.grade = NodeHealthGrade::YELLOW;
            wallet_health.status = "No wallet loaded";
            wallet_health.warnings.push_back("Wallet not loaded (receive/send not available)");
            if (health.overall_grade == NodeHealthGrade::GREEN) {
                health.overall_grade = NodeHealthGrade::YELLOW;
            }
        }

        health.subsystems.push_back(wallet_health);
    }

    // ========================================================================
    // Subsystem 4: Mempool Congestion
    // ========================================================================
    {
        SubsystemHealth mempool_health;
        mempool_health.name = "mempool";

        if (ctx.mempool_v2 != nullptr) {
            // Build fee histogram from mempool
            FeeHistogram histogram;
            histogram.BuildFromMempool(ctx.mempool_v2);

            auto stats = ctx.mempool_v2->getStats();
            size_t tx_count = stats.tx_count;

            if (tx_count == 0) {
                // Empty mempool
                mempool_health.grade = NodeHealthGrade::GREEN;
                mempool_health.status = "Empty (0 txs)";
            } else {
                // Get percentiles for congestion assessment
                // p25 = 25th percentile feerate (una/vB)
                uint64_t p10 = histogram.GetFeeRateAtPercentile(0.10);
                uint64_t p25 = histogram.GetFeeRateAtPercentile(0.25);
                uint64_t p50 = histogram.GetFeeRateAtPercentile(0.50);
                uint64_t p75 = histogram.GetFeeRateAtPercentile(0.75);

                // Congestion classification based on 25th percentile:
                //   p25 < 2 una/vB  → LOW (green)
                //   p25 2-10        → MODERATE (yellow)
                //   p25 > 10        → HIGH (red)
                if (p25 < 2) {
                    mempool_health.grade = NodeHealthGrade::GREEN;
                    mempool_health.status = "Low congestion (" + std::to_string(tx_count) + " txs)";
                } else if (p25 <= 10) {
                    mempool_health.grade = NodeHealthGrade::YELLOW;
                    mempool_health.status = "Moderate congestion (" + std::to_string(tx_count) + " txs)";
                    mempool_health.warnings.push_back("Fee pressure elevated, recommend higher fees");
                    // Don't downgrade overall health for moderate congestion
                } else {
                    mempool_health.grade = NodeHealthGrade::RED;
                    mempool_health.status = "High congestion (" + std::to_string(tx_count) + " txs)";
                    mempool_health.warnings.push_back("Significant fee pressure, expect delays for low-fee txs");
                    // Don't downgrade overall health for congestion (it's informational)
                }

                // Add percentile details as warnings (informational)
                mempool_health.warnings.push_back(
                    "Fee percentiles (una/vB): p10=" + std::to_string(p10) +
                    " p25=" + std::to_string(p25) +
                    " p50=" + std::to_string(p50) +
                    " p75=" + std::to_string(p75)
                );
            }
        } else {
            mempool_health.grade = NodeHealthGrade::YELLOW;
            mempool_health.status = "Mempool unavailable";
            mempool_health.warnings.push_back("Cannot assess congestion");
        }

        health.subsystems.push_back(mempool_health);
    }

    // ========================================================================
    // Generate Recommendations
    // ========================================================================
    if (health.overall_grade == NodeHealthGrade::GREEN) {
        health.recommendations.push_back("Node is healthy and ready for production use");
    } else if (health.overall_grade == NodeHealthGrade::YELLOW) {
        health.recommendations.push_back("Node is functional but has minor issues");
        health.recommendations.push_back("Review subsystem warnings for details");
    } else {
        health.recommendations.push_back("Node has critical issues and is not ready");
        health.recommendations.push_back("Wait for sync to complete or resolve errors");
    }

    return health;
}

// ============================================================================
// Build MiningReadiness from system state
// ============================================================================

MiningReadiness BuildMiningReadiness(const ExecutionContext& ctx) {
    MiningReadiness readiness;
    readiness.timestamp_ms = GetCurrentTimeMs();

    // Initialize all checks as false
    readiness.node_synced = false;
    readiness.wallet_synced = false;
    readiness.sufficient_peers = false;
    readiness.no_active_reorg = false;
    readiness.wallet_loaded = false;

    try {
        // Check 1: Node sync status
        WalletSyncStatus sync_status = BuildWalletSyncStatusFromContext(ctx);

        readiness.node_synced = (sync_status.phase == SyncPhase::STEADY_STATE &&
                                 sync_status.overall_progress >= 0.99);
        if (!readiness.node_synced) {
            readiness.blockers.push_back("Node not fully synced (phase: " +
                std::to_string(static_cast<int>(sync_status.phase)) +
                ", progress: " + std::to_string(static_cast<int>(sync_status.overall_progress * 100)) + "%)");
        }

        // Check 2: Wallet sync status
        readiness.wallet_loaded = (ctx.wallet_manager != nullptr);
        if (readiness.wallet_loaded) {
            readiness.wallet_synced = (sync_status.wallet_scan_height >= sync_status.chain_height);
            if (!readiness.wallet_synced) {
                readiness.blockers.push_back("Wallet scanning in progress (" +
                    std::to_string(sync_status.wallet_scan_height) + " / " +
                    std::to_string(sync_status.chain_height) + ")");
            }
        } else {
            readiness.blockers.push_back("Wallet not loaded (use loadwallet)");
        }

        // Check 3: Network connectivity
        int peer_count = 0;
        if (ctx.daemon && ctx.daemon->p2p) {
            peer_count = static_cast<int>(ctx.daemon->p2p->GetPeerCount());
        }
        readiness.sufficient_peers = (peer_count >= 3);
        if (!readiness.sufficient_peers) {
            readiness.blockers.push_back("Insufficient peers (" + std::to_string(peer_count) +
                " connected, need 3+)");
        } else if (peer_count < 8) {
            readiness.warnings.push_back("Low peer count (" + std::to_string(peer_count) +
                "), recommend 8+ for better relay");
        }

        // Check 4: No active reorg
        readiness.no_active_reorg = !sync_status.is_reorg_in_progress;
        if (!readiness.no_active_reorg) {
            readiness.blockers.push_back("Active reorg in progress (depth: " +
                std::to_string(sync_status.last_reorg_depth) + ")");
        }

    } catch (const std::exception& e) {
        readiness.blockers.push_back(std::string("Failed to check mining readiness: ") + e.what());
    }

    // Determine overall readiness
    readiness.ready = (readiness.node_synced &&
                       readiness.wallet_synced &&
                       readiness.sufficient_peers &&
                       readiness.no_active_reorg &&
                       readiness.wallet_loaded);

    // Generate recommendation
    if (readiness.ready) {
        readiness.recommendation = "Node is ready to mine. Use getblocktemplate to start.";
    } else if (!readiness.blockers.empty()) {
        readiness.recommendation = "Resolve blockers before mining: " +
            std::to_string(readiness.blockers.size()) + " issue(s) found.";
    } else {
        readiness.recommendation = "Unknown mining readiness state.";
    }

    return readiness;
}

// ============================================================================
// JSON Conversion
// ============================================================================

din::Json NodeHealthToJson(const NodeHealth& health) {
    din::Json result;

    result["overall_grade"] = NodeHealthGradeToString(health.overall_grade);
    result["timestamp_ms"] = static_cast<Json::Value::Int64>(health.timestamp_ms);

    // Subsystems
    din::Json subsystems_array(Json::arrayValue);
    for (const auto& subsystem : health.subsystems) {
        din::Json sub;
        sub["name"] = subsystem.name;
        sub["grade"] = NodeHealthGradeToString(subsystem.grade);
        sub["status"] = subsystem.status;

        din::Json warnings_array(Json::arrayValue);
        for (const auto& warning : subsystem.warnings) {
            warnings_array.append(warning);
        }
        sub["warnings"] = warnings_array;

        subsystems_array.append(sub);
    }
    result["subsystems"] = subsystems_array;

    // Recommendations
    din::Json recommendations_array(Json::arrayValue);
    for (const auto& rec : health.recommendations) {
        recommendations_array.append(rec);
    }
    result["recommendations"] = recommendations_array;

    return result;
}

din::Json MiningReadinessToJson(const MiningReadiness& readiness) {
    din::Json result;

    result["ready"] = readiness.ready;
    result["timestamp_ms"] = static_cast<Json::Value::Int64>(readiness.timestamp_ms);
    result["recommendation"] = readiness.recommendation;

    // Blockers
    din::Json blockers_array(Json::arrayValue);
    for (const auto& blocker : readiness.blockers) {
        blockers_array.append(blocker);
    }
    result["blockers"] = blockers_array;

    // Warnings
    din::Json warnings_array(Json::arrayValue);
    for (const auto& warning : readiness.warnings) {
        warnings_array.append(warning);
    }
    result["warnings"] = warnings_array;

    // Detailed checks
    din::Json checks;
    checks["node_synced"] = readiness.node_synced;
    checks["wallet_synced"] = readiness.wallet_synced;
    checks["sufficient_peers"] = readiness.sufficient_peers;
    checks["no_active_reorg"] = readiness.no_active_reorg;
    checks["wallet_loaded"] = readiness.wallet_loaded;
    result["checks"] = checks;

    return result;
}

// ============================================================================
// RPC Handlers
// ============================================================================

std::string RPC_GetNodeHealth(const ExecutionContext& ctx) {
    try {
        NodeHealth health = BuildNodeHealth(ctx);
        din::Json result = NodeHealthToJson(health);

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, result);

    } catch (const std::exception& e) {
        dinero::g_logger.error("RPC_GetNodeHealth failed: " + std::string(e.what()));

        din::Json error_result;
        error_result["error"] = "Failed to check node health";
        error_result["details"] = e.what();

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, error_result);
    }
}

std::string RPC_GetMiningReadiness(const ExecutionContext& ctx) {
    try {
        MiningReadiness readiness = BuildMiningReadiness(ctx);
        din::Json result = MiningReadinessToJson(readiness);

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, result);

    } catch (const std::exception& e) {
        dinero::g_logger.error("RPC_GetMiningReadiness failed: " + std::string(e.what()));

        din::Json error_result;
        error_result["error"] = "Failed to check mining readiness";
        error_result["details"] = e.what();

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, error_result);
    }
}

// ============================================================================
// RPC Registry Integration
// ============================================================================

/**
 * Adapter: Convert RPC_GetNodeHealth (returns string) to RpcHandler signature
 */
din::Json rpc_getnodehealth(const ExecutionContext& ctx, const din::Json& params) {
    try {
        std::string json_str = RPC_GetNodeHealth(ctx);

        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        din::Json result;
        std::string errors;

        bool success = reader->parse(
            json_str.c_str(),
            json_str.c_str() + json_str.size(),
            &result,
            &errors
        );
        delete reader;

        if (!success) {
            din::Json error;
            error["error"] = "Failed to parse node health response";
            error["details"] = errors;
            return error;
        }

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = "getnodehealth failed";
        error["details"] = e.what();
        return error;
    }
}

/**
 * Adapter: Convert RPC_GetMiningReadiness (returns string) to RpcHandler signature
 */
din::Json rpc_getminingreadiness(const ExecutionContext& ctx, const din::Json& params) {
    try {
        std::string json_str = RPC_GetMiningReadiness(ctx);

        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        din::Json result;
        std::string errors;

        bool success = reader->parse(
            json_str.c_str(),
            json_str.c_str() + json_str.size(),
            &result,
            &errors
        );
        delete reader;

        if (!success) {
            din::Json error;
            error["error"] = "Failed to parse mining readiness response";
            error["details"] = errors;
            return error;
        }

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = "getminingreadiness failed";
        error["details"] = e.what();
        return error;
    }
}

} // namespace dinero

// ============================================================================
// RPC Method Registration (Global Namespace)
// ============================================================================

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

void registerErgonomicsRpcMethods() {
    g_rpcRegistry.registerHandler("node.gethealth",
                                 dinero::rpc_getnodehealth,
                                 RegisterMode::Overwrite,
                                 "ergonomics");

    g_rpcRegistry.registerHandler("mining.getreadiness",
                                 dinero::rpc_getminingreadiness,
                                 RegisterMode::Overwrite,
                                 "ergonomics");

    dinero::g_logger.info("Registered Phase W.3 RPC methods: node.gethealth, mining.getreadiness");
}

} // namespace rpc
} // namespace dinero
