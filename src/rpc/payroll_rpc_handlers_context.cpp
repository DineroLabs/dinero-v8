/**
 * Payroll RPC Context Handlers (legacy shim)
 *
 * These handlers remain registered for compatibility but intentionally reject
 * requests when the dedicated payroll backend is not wired.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void ThrowPayrollUnavailable(const char* method) {
    throw std::runtime_error(
        std::string(method) +
        " unavailable: payroll backend bridge is not configured for context handlers");
}

}  // namespace

/** payroll.createinvoice */
din::Json rpc_context_payroll_createinvoice(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    ThrowPayrollUnavailable("payroll.createinvoice");
}

/** payroll.payinvoice */
din::Json rpc_context_payroll_payinvoice(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    ThrowPayrollUnavailable("payroll.payinvoice");
}

/** payroll.payall */
din::Json rpc_context_payroll_payall(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    ThrowPayrollUnavailable("payroll.payall");
}

/** payroll.report */
din::Json rpc_context_payroll_report(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    ThrowPayrollUnavailable("payroll.report");
}

/** payroll.listinvoices */
din::Json rpc_context_payroll_listinvoices(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    ThrowPayrollUnavailable("payroll.listinvoices");
}

/** Register all payroll RPC methods */
void WirePayrollRpcContext() {
    using namespace din;

    RpcRegistry::instance().registerContextMethod(
        "payroll.createinvoice",
        rpc_context_payroll_createinvoice,
        "Create payroll invoice for employee"
    );

    RpcRegistry::instance().registerContextMethod(
        "payroll.payinvoice",
        rpc_context_payroll_payinvoice,
        "Pay a single payroll invoice"
    );

    RpcRegistry::instance().registerContextMethod(
        "payroll.payall",
        rpc_context_payroll_payall,
        "Batch pay all unpaid invoices for period"
    );

    RpcRegistry::instance().registerContextMethod(
        "payroll.report",
        rpc_context_payroll_report,
        "Generate payroll report for period"
    );

    RpcRegistry::instance().registerContextMethod(
        "payroll.listinvoices",
        rpc_context_payroll_listinvoices,
        "List invoices with optional filters"
    );
}
