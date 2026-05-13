/**
 * vNext RPC Method Registration - TEST FILE
 *
 * This demonstrates the new RPC_METHOD DSL with full metadata.
 * Once verified, this pattern will be applied to all 170+ methods.
 */

#include "rpc/rpc_method_builder.h"
#include "din_json.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/daemon_context.h"
#include <stdexcept>

// Test method: blockchain.getblockcount
// This will be migrated from old registerHandler() style

namespace {

// Implementation function (unchanged from before)
din::Json getblockcount_vnext_impl(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        throw std::runtime_error("chainstate service not available");
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        throw std::runtime_error("failed to resolve chainstate service");
    }

    dinero::ChainDB* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        throw std::runtime_error("chain database not available");
    }

    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        throw std::runtime_error("failed to read active chain tip");
    }

    return din::Json(static_cast<int>(tip_result.value().height));
}

// NEW: vNext registration with full metadata
void register_test_methods_vnext() {
    RPC_METHOD("blockchain.getblockcount", "blockchain")
        .description("Returns the current number of blocks in the blockchain")
        .params({})  // No parameters
        .result("number", "The current block height")
        .handler(getblockcount_vnext_impl)
        .examples({
            "blockchain.getblockcount"
        });
}

} // anonymous namespace

// Auto-register at startup
static auto _vnext_test_init = (register_test_methods_vnext(), 0);
