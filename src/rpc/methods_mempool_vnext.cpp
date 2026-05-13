/**
 * Mempool RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Memory pool information and fee estimation.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_mempool.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_mempool.cpp
extern din::Json rpc_getmempoolinfo(const ExecutionContext& ctx, const din::Json& params);
extern din::Json estimatefee_impl(const ExecutionContext& ctx, const din::Json& params);

void registerMempoolMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // MEMPOOL & FEE ESTIMATION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("getmempoolinfo", "mempool")
        .description("Returns memory pool information and statistics")
        .params({})
        .result("object", "Mempool info including size, bytes, fees, and transaction count")
        .handler(rpc_getmempoolinfo)
        .examples({
            "getmempoolinfo"
        });

    RPC_METHOD("estimatefee", "mempool")
        .description("Estimates transaction fee for target confirmation blocks")
        .param("blocks", "number", "Target blocks for confirmation (default: 6)", false)
        .result("object", "Fee estimate in una per vbyte and per kB")
        .handler(estimatefee_impl)
        .examples({
            "estimatefee",
            "estimatefee 1",
            "estimatefee 10"
        });

    dinero::g_logger.info("✅ Registered 2 mempool methods (vNext DSL)");
}

// Auto-register at program startup
static auto _mempool_vnext_init = (din::rpc::registerMempoolMethodsVNext(), 0);

} // namespace rpc
} // namespace din
