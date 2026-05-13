/**
 * Sync RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Blockchain synchronization status and state.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_sync.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_sync.cpp and consensus_rpc_handlers.cpp
extern din::Json getsyncstate_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_getreorgstatus(const ExecutionContext& ctx, const din::Json& params);

void registerSyncMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // BLOCKCHAIN SYNC STATUS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("sync.getsyncstate", "sync")
        .description("Returns detailed blockchain synchronization state")
        .params({})
        .result("object", "Sync state including current height, target height, progress percentage, and sync phase")
        .handler(getsyncstate_impl)
        .examples({
            "sync.getsyncstate"
        });

    RPC_METHOD("sync.getreorgstatus", "sync")
        .description("Returns blockchain reorganization status and recent reorg events")
        .params({})
        .result("object", "Reorg status including last reorg depth, count, and affected blocks")
        .handler(rpc_getreorgstatus)
        .examples({
            "sync.getreorgstatus"
        });

    dinero::g_logger.info("✅ Registered 2 sync methods (vNext DSL)");
}

// Auto-register at program startup
static auto _sync_vnext_init = (din::rpc::registerSyncMethodsVNext(), 0);

} // namespace rpc
} // namespace din
