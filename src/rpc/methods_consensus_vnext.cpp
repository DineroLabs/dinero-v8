/**
 * Consensus RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Consensus validation and database integrity checks.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_consensus.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_consensus.cpp
extern din::Json consensus_checkdb_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getverificationsummary_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getchainwork_impl(const ExecutionContext& ctx, const din::Json& params);

void registerConsensusMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // CONSENSUS VALIDATION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("consensus.checkdb", "consensus")
        .description("Validates consensus database integrity and consistency")
        .param("level", "number", "Check level: 0=quick, 1=standard, 2=deep (default: 1)", false)
        .result("object", "Validation results including errors found and recommendations")
        .handler(consensus_checkdb_impl)
        .examples({
            "consensus.checkdb",
            "consensus.checkdb 2"
        });

    RPC_METHOD("getverificationsummary", "consensus")
        .description("Returns summary of block verification status")
        .params({})
        .result("object", "Verification summary including verified blocks, pending, and failed")
        .handler(getverificationsummary_impl)
        .examples({
            "getverificationsummary"
        });

    RPC_METHOD("getchainwork", "consensus")
        .description("Returns the total work (cumulative difficulty) in the blockchain")
        .params({})
        .result("string", "Total chain work as hexadecimal string")
        .handler(getchainwork_impl)
        .examples({
            "getchainwork"
        });

    dinero::g_logger.info("✅ Registered 3 consensus methods (vNext DSL)");
}

// Auto-register at program startup
static auto _consensus_vnext_init = (din::rpc::registerConsensusMethodsVNext(), 0);

} // namespace rpc
} // namespace din
