/**
 * Economics RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Economic policy, supply statistics, and monetary information.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_economics.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Global namespace declarations
extern RpcRegistry g_rpcRegistry;  // Lives in global namespace, NOT din::rpc!

} // namespace rpc
} // namespace din

// Implementation functions from methods_economics_context.cpp (global namespace)
extern din::Json rpc_context_economics_getsupply(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_economics_getinfo(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_economics_getminerstats(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getverificationsummary(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_consensus_checkdb(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_rpc_version(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getcontext(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_rpc_listmethods(const ExecutionContext& ctx, const din::Json& params);

namespace din {
namespace rpc {

void registerEconomicsMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // ECONOMIC POLICY & SUPPLY
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("economics.getsupply", "economics")
        .description("Returns detailed supply statistics including PoW issuance and tail emission")
        .params({})
        .result("object", "Supply info with total_issued, current_block_reward, and emission schedule")
        .handler(rpc_context_economics_getsupply)
        .examples({
            "getsupply"
        });

    RPC_METHOD("economics.getinfo", "economics")
        .description("Returns comprehensive economic and monetary policy information")
        .params({})
        .result("object", "Economic policy including emission schedule, halving epochs, and monetary rules")
        .handler(rpc_context_economics_getinfo)
        .examples({
            "geteconomics"
        });

    RPC_METHOD("economics.getminerstats", "economics")
        .description("Returns mining statistics and difficulty metrics")
        .params({})
        .result("object", "Mining stats including difficulty, hashrate estimates, and block time averages")
        .handler(rpc_context_economics_getminerstats)
        .examples({
            "getminerstats"
        });

    // ═══════════════════════════════════════════════════════════════
    // VERIFICATION & INTROSPECTION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("telemetry.getverificationsummary", "telemetry")
        .description("Returns block verification status summary")
        .params({})
        .result("object", "Verification summary including verified, pending, and failed block counts")
        .handler(rpc_context_getverificationsummary)
        .examples({
            "getverificationsummary"
        });

    RPC_METHOD("consensus.checkdb", "consensus")
        .description("Validates database consistency and consensus rules")
        .param("level", "number", "Check level: 0=quick, 1=standard, 2=deep (default: 1)", false)
        .result("object", "Validation results with errors found and recommendations")
        .handler(rpc_context_consensus_checkdb)
        .examples({
            "consensus.checkdb",
            "consensus.checkdb 2"
        });

    RPC_METHOD("rpc.version", "rpc")
        .description("Returns RPC API version information")
        .params({})
        .result("object", "RPC version with major, minor, and patch numbers")
        .handler(rpc_context_rpc_version)
        .examples({
            "rpc.version"
        });

    RPC_METHOD("rpc.getcontext", "rpc")
        .description("Returns daemon identity context for authentication verification")
        .params({})
        .result("object", "Daemon context including network, datadir, rpcport, wallet, and daemon_id")
        .handler(rpc_context_getcontext)
        .examples({
            "rpc.getcontext"
        });

    RPC_METHOD("rpc.listmethods", "rpc")
        .description("Returns list of all available RPC methods")
        .params({})
        .result("array", "Array of method names")
        .handler(rpc_context_rpc_listmethods)
        .examples({
            "rpc.listmethods"
        });

    dinero::g_logger.info("✅ Registered 8 economics methods (vNext DSL)");
}

// Note: Explicitly called from main.cpp (not using static initializer due to static library linking issues)

} // namespace rpc
} // namespace din
