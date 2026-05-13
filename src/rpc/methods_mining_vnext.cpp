/**
 * Mining RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Mining control, configuration, and statistics.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_mining.h"
#include "common/logger.h"
#include "daemon/daemon_context.h"  // For DaemonContext complete type
#include "daemon/services/chainstate_service.h"  // For ChainstateService complete type
#include "daemon/services/mining_service.h"      // For MiningService complete type
// NOTE: Stratum removed from dinerod - use separate dinero-stratum binary
// #include "stratum/stratum_server.h"  // For StratumServer complete type
#include "mining/header_layout.h"    // Dinero 128-byte header constants (BlockHeader v1)
#include "mining/midstate_cache.h"   // SHA256 midstate for Stratum mining
#include "consensus/pow.hpp"         // GetNextWorkRequired
#include "consensus/consensus.hpp"   // Consensus parameters
#include "storage/chain_db.h"        // ChainDB for difficulty calculation
#include "consensus/chainparams.h"   // dinero::Params()
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>

namespace din {
namespace rpc {

// Implementation functions from methods_mining.cpp (with extra parameters)
// These need to be wrapped in lambdas that capture the required state

#if 0  // DISABLED: registerMiningMethodsVNext() not currently used
       // Only registerStratumMethodsContext() is called from rpc_context_wiring.cpp
void registerMiningMethodsVNext(
    std::shared_ptr<dinero::rpc::MiningState> mining_state,
    const dinero::rpc::MiningConfig& config,
    std::function<bool(const std::string&)> registerMiningAddressCallback,
    std::shared_ptr<std::shared_ptr<HDWallet>> g_hd_wallet
) {
    // Declare implementation functions (defined in methods_mining.cpp)
    extern din::Json rpc_mining_info(
        const ExecutionContext& ctx,
        const din::Json& params,
        std::shared_ptr<dinero::rpc::MiningState> mining_state
    );
    extern din::Json rpc_mining_start(
        const ExecutionContext& ctx,
        const din::Json& params,
        std::shared_ptr<dinero::rpc::MiningState> mining_state,
        const dinero::rpc::MiningConfig& config,
        std::function<bool(const std::string&)> registerMiningAddressCallback
    );
    extern din::Json rpc_mining_stop(
        const ExecutionContext& ctx,
        const din::Json& params,
        std::shared_ptr<dinero::rpc::MiningState> mining_state
    );
    extern din::Json rpc_mining_setaddress(
        const ExecutionContext& ctx,
        const din::Json& params,
        std::shared_ptr<dinero::rpc::MiningState> mining_state,
        const dinero::rpc::MiningConfig& config,
        std::function<bool(const std::string&)> registerMiningAddressCallback,
        std::shared_ptr<std::shared_ptr<HDWallet>> g_hd_wallet
    );
    extern din::Json rpc_mining_getaddress(
        const ExecutionContext& ctx,
        const din::Json& params,
        std::shared_ptr<dinero::rpc::MiningState> mining_state,
        const dinero::rpc::MiningConfig& config
    );

    // ═══════════════════════════════════════════════════════════════
    // MINING CONTROL
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("mining.info", "mining")
        .description("Returns current mining status, hashrate, and statistics")
        .params({})
        .result("object", "Mining info including enabled status, thread count, hashrate, and blocks found")
        .handler([mining_state](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_mining_info(ctx, params, mining_state);
        })
        .examples({
            "mining.info"
        });

    RPC_METHOD("mining.start", "mining")
        .description("Start mining with specified number of threads")
        .param("threads", "number", "Number of mining threads (default: 1)", false)
        .result("object", "Mining start confirmation with thread count and address")
        .handler([mining_state, config, registerMiningAddressCallback](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_mining_start(ctx, params, mining_state, config, registerMiningAddressCallback);
        })
        .examples({
            "mining.start",
            "mining.start 4"
        });

    RPC_METHOD("mining.stop", "mining")
        .description("Stop mining operations")
        .params({})
        .result("object", "Mining stop confirmation")
        .handler([mining_state](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_mining_stop(ctx, params, mining_state);
        })
        .examples({
            "mining.stop"
        });

    RPC_METHOD("mining.setaddress", "mining")
        .description("Set the mining payout address")
        .param("address", "string", "Dinero address to receive mining rewards", true)
        .result("object", "Address set confirmation")
        .handler([mining_state, config, registerMiningAddressCallback, g_hd_wallet](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_mining_setaddress(ctx, params, mining_state, config, registerMiningAddressCallback, g_hd_wallet);
        })
        .examples({
            "mining.setaddress \"din1q...\""
        });

    RPC_METHOD("mining.getaddress", "mining")
        .description("Get the current mining payout address")
        .params({})
        .result("object", "Current mining address")
        .handler([mining_state, config](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_mining_getaddress(ctx, params, mining_state, config);
        })
        .examples({
            "mining.getaddress"
        });

    // Note: mining.getstratuminfo is registered separately via registerStratumMethodsContext()
    // because it doesn't require MiningState - it only needs DaemonContext

    dinero::g_logger.info("✅ Registered 5 mining methods (vNext DSL)");
}
#endif // DISABLED registerMiningMethodsVNext()

// Standalone registration for context-only methods (no MiningState required)
// This registers methods that only need ExecutionContext (DaemonContext access)
void registerStratumMethodsContext() {
    // ═══════════════════════════════════════════════════════════════════════════
    // NOTE: Stratum server removed from dinerod - use separate dinero-stratum binary
    // This RPC method now returns information about the separation
    // ═══════════════════════════════════════════════════════════════════════════
    RPC_METHOD("mining.getstratuminfo", "mining")
        .description("Get Stratum V1 mining server status (NOTE: Stratum is now a separate binary)")
        .params({})
        .result("object", "Information about Stratum server separation")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();

            // Stratum is now a separate binary - inform users
            result["status"] = "separated";
            result["message"] = "Stratum server removed from dinerod. Use separate dinero-stratum binary.";
            result["binary"] = "dinero-stratum";
            result["usage"] = "dinero-stratum --rpcport=20998 --stratumport=3333";
            result["reason"] = "Security isolation: Stratum attack surface separated from consensus";

            return result;
        })
        .examples({
            "mining.getstratuminfo"
        });

    // ═══════════════════════════════════════════════════════════════════════════
    // mining.getstratumtemplate - Stratum V1 template for external miners
    // ═══════════════════════════════════════════════════════════════════════════
    RPC_METHOD("mining.getstratumtemplate", "mining")
        .description("DISABLED: Stratum templates are served by standalone dinero-stratum (this endpoint previously returned invalid synthetic templates)")
        .params({})
        .result("object", "Disabled response with guidance to use dinero-stratum standalone")
        .handler([](const ExecutionContext& ctx, const din::Json& params) {
            din::Json result = din::obj();

            (void)ctx;
            (void)params;

            // Fail closed: this endpoint previously returned invalid synthetic templates (zero merkle/utreexo).
            // The production mining architecture uses the standalone `dinero-stratum` binary.
            result["status"] = "disabled";
            result["error"] = "mining.getstratumtemplate is disabled";
            result["message"] = "Stratum templates are served by the standalone dinero-stratum binary. Use getblocktemplate + dinero-stratum instead.";
            result["binary"] = "dinero-stratum";
            result["usage"] = "dinero-stratum --rpcport=20998 --stratumport=3333";
            result["rpc_schema"] = "din.mining.v1";
            return result;
        })
        .examples({
            "mining.getstratumtemplate"
        });

    dinero::g_logger.info("✅ Registered mining template methods (stratum is separate binary)");
}

// Auto-register at program startup (requires calling from main with parameters)
// Note: Cannot use static initializer here due to required parameters

} // namespace rpc
} // namespace din
