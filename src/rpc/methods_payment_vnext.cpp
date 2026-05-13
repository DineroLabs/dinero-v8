/**
 * Payment RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Payment monitoring and transaction analysis.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_payment.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_payment.cpp
extern din::Json payment_watch_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json payment_status_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json payment_unwatch_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json payment_list_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json payment_analyze_impl(const ExecutionContext& ctx, const din::Json& params);

void registerPaymentMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // PAYMENT MONITORING
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("payment.watch", "payment")
        .description("Monitors a payment address for incoming transactions")
        .param("address", "string", "Address to monitor", true)
        .param("confirmations", "number", "Required confirmations (default: 1)", false)
        .param("webhook_url", "string", "Optional webhook URL for notifications", false)
        .result("object", "Watch ID and monitoring parameters")
        .handler(payment_watch_impl)
        .examples({
            "payment.watch \"din1q...\"",
            "payment.watch \"din1q...\" 6",
            "payment.watch \"din1q...\" 1 \"https://example.com/webhook\""
        });

    RPC_METHOD("payment.status", "payment")
        .description("Returns payment monitoring status for an address or watch ID")
        .param("identifier", "string", "Watch ID or address", true)
        .result("object", "Payment status including received amount, confirmations, and transaction IDs")
        .handler(payment_status_impl)
        .examples({
            "payment.status \"watch_abc123...\"",
            "payment.status \"din1q...\""
        });

    RPC_METHOD("payment.unwatch", "payment")
        .description("Stops monitoring a payment address")
        .param("identifier", "string", "Watch ID or address to stop monitoring", true)
        .result("object", "Unwatch confirmation")
        .handler(payment_unwatch_impl)
        .examples({
            "payment.unwatch \"watch_abc123...\"",
            "payment.unwatch \"din1q...\""
        });

    RPC_METHOD("payment.analyze", "payment")
        .description("Analyzes payment transaction details including fees and routing")
        .param("txid", "string", "Transaction ID to analyze", true)
        .result("object", "Payment analysis including inputs, outputs, fees, and confirmations")
        .handler(payment_analyze_impl)
        .examples({
            "payment.analyze \"abc123...\""
        });

    dinero::g_logger.info("✅ Registered 4 payment methods (vNext DSL)");
}

// Auto-register at program startup
static auto _payment_vnext_init = (din::rpc::registerPaymentMethodsVNext(), 0);

} // namespace rpc
} // namespace din
