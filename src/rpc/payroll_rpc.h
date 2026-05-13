#pragma once

#include "database/payroll_db.h"
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include <memory>
#include <string>

namespace dinero::rpc {

/**
 * PayrollRPC - RPC interface for Private Payroll over Lightning
 *
 * Phase 1: Transparent Lightning payments
 * Phase 2: Add confidential commitments (when ZK Phase A/B complete)
 * Phase 3: Add aggregated proofs and auditing
 *
 * Methods:
 *   - payroll.createinvoice - Create payroll invoice for employee
 *   - payroll.payinvoice    - Pay a single invoice
 *   - payroll.payall        - Batch pay all unpaid invoices
 *   - payroll.report        - Generate payroll report
 *   - payroll.listinvoices  - List invoices with filters
 */
class PayrollRPC {
public:
    /**
     * Constructor
     * @param db Shared pointer to PayrollDB instance
     */
    explicit PayrollRPC(std::shared_ptr<dinero::payroll::PayrollDB> db);

    /**
     * Register all payroll RPC methods with the registry
     */
    void Register();

private:
    std::shared_ptr<dinero::payroll::PayrollDB> db_;

    // RPC method handlers
    din::Json CreateInvoice(const ExecutionContext& ctx, const din::Json& params);
    din::Json PayInvoice(const ExecutionContext& ctx, const din::Json& params);
    din::Json PayAll(const ExecutionContext& ctx, const din::Json& params);
    din::Json Report(const ExecutionContext& ctx, const din::Json& params);
    din::Json ListInvoices(const ExecutionContext& ctx, const din::Json& params);

    // Helper methods
    std::string GenerateInvoiceID();
    std::string GetCurrentPayPeriod();
    std::string HashEmployeeID(const std::string& employee_id);

    // Lightning integration (Phase 1: mocked, Phase 2: real)
    din::Json CreateLightningInvoice(uint64_t amount_una, const std::string& memo);
    din::Json PayLightningInvoice(const std::string& ln_invoice);
};

/**
 * Global registration function - called from WirePayrollRpcContext()
 */
void WirePayrollRpcContext(std::shared_ptr<dinero::payroll::PayrollDB> db);

} // namespace dinero::rpc
