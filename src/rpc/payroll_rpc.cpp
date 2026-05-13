#include "rpc/payroll_rpc.h"
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>

namespace dinero::rpc {

// ============================================================================
// Constructor
// ============================================================================

PayrollRPC::PayrollRPC(std::shared_ptr<dinero::payroll::PayrollDB> db)
    : db_(db) {
    if (!db_) {
        throw std::runtime_error("[PayrollRPC] Database pointer is null");
    }
}

// ============================================================================
// Registration
// ============================================================================

void PayrollRPC::Register() {
    g_rpcRegistry.registerHandler(
        "payroll.createinvoice",
        [this](const ExecutionContext& ctx, const din::Json& params) {
            return CreateInvoice(ctx, params);
        },
        RegisterMode::Overwrite,
        "Create payroll invoice for employee"
    );

    g_rpcRegistry.registerHandler(
        "payroll.payinvoice",
        [this](const ExecutionContext& ctx, const din::Json& params) {
            return PayInvoice(ctx, params);
        },
        RegisterMode::Overwrite,
        "Pay a single payroll invoice"
    );

    g_rpcRegistry.registerHandler(
        "payroll.payall",
        [this](const ExecutionContext& ctx, const din::Json& params) {
            return PayAll(ctx, params);
        },
        RegisterMode::Overwrite,
        "Batch pay all unpaid invoices for period"
    );

    g_rpcRegistry.registerHandler(
        "payroll.report",
        [this](const ExecutionContext& ctx, const din::Json& params) {
            return Report(ctx, params);
        },
        RegisterMode::Overwrite,
        "Generate payroll report for period"
    );

    g_rpcRegistry.registerHandler(
        "payroll.listinvoices",
        [this](const ExecutionContext& ctx, const din::Json& params) {
            return ListInvoices(ctx, params);
        },
        RegisterMode::Overwrite,
        "List invoices with optional filters"
    );

    dinero::g_logger.info("[PayrollRPC] Registered 5 payroll RPC methods");
}

// ============================================================================
// RPC Method Handlers
// ============================================================================

din::Json PayrollRPC::CreateInvoice(const ExecutionContext& ctx, const din::Json& params) {
    // Validate parameters
    if (!params.isMember("employee_id")) {
        throw std::runtime_error("Missing required parameter: employee_id");
    }
    if (!params.isMember("amount")) {
        throw std::runtime_error("Missing required parameter: amount");
    }

    std::string employee_id = params["employee_id"].asString();
    std::string employee_id_hash = HashEmployeeID(employee_id);
    double amount_din = params["amount"].asDouble();
    uint64_t amount_una = static_cast<uint64_t>(amount_din * 100000000);
    std::string memo = params.get("memo", Json::Value("Payroll")).asString();
    std::string pay_period = params.get("pay_period", Json::Value(GetCurrentPayPeriod())).asString();

    dinero::g_logger.info("[PayrollRPC] Creating invoice for " + employee_id +
                          " amount=" + std::to_string(amount_din) + " DIN period=" + pay_period);

    // Generate invoice ID
    std::string invoice_id = GenerateInvoiceID();

    // Create Lightning invoice via the configured backend bridge.
    auto ln_result = CreateLightningInvoice(amount_una, memo + " - " + employee_id);

    // Create PayrollInvoice record
    dinero::payroll::PayrollInvoice invoice;
    invoice.employee_id_hash = employee_id_hash;
    invoice.pay_period = pay_period;
    invoice.lightning_invoice = ln_result["invoice"].asString();
    invoice.payment_hash = ln_result["payment_hash"].asString();
    invoice.gross_commitment = "";  // Phase 2: will contain Pedersen commitment
    invoice.range_proof = "";       // Phase 2: will contain Bulletproof
    invoice.view_nonce = "";        // Phase 2: will contain view nonce
    invoice.timestamp = std::time(nullptr);
    invoice.paid = false;
    invoice.payment_preimage = "";
    invoice.paid_at = 0;

    // Store in database
    if (!db_->StoreInvoice(invoice)) {
        throw std::runtime_error("Failed to store invoice in database");
    }

    // Return invoice details
    din::Json result;
    result["invoice_id"] = invoice_id;
    result["employee_id_hash"] = employee_id_hash;
    result["pay_period"] = pay_period;
    result["amount"] = amount_din;
    result["amount_una"] = Json::Value::UInt64(amount_una);
    result["ln_invoice"] = invoice.lightning_invoice;
    result["payment_hash"] = invoice.payment_hash;
    result["expires_at"] = ln_result["expires_at"];
    result["memo"] = memo;

    dinero::g_logger.info("[PayrollRPC] Invoice created: " + invoice_id);

    return result;
}

din::Json PayrollRPC::PayInvoice(const ExecutionContext& ctx, const din::Json& params) {
    if (!params.isMember("employee_id_hash") || !params.isMember("pay_period")) {
        throw std::runtime_error("Missing required parameters: employee_id_hash, pay_period");
    }

    std::string employee_id_hash = params["employee_id_hash"].asString();
    std::string pay_period = params["pay_period"].asString();

    dinero::g_logger.info("[PayrollRPC] Paying invoice: " + employee_id_hash + " period=" + pay_period);

    // Fetch invoice from database
    auto invoice_opt = db_->GetInvoice(pay_period, employee_id_hash);
    if (!invoice_opt) {
        throw std::runtime_error("Invoice not found");
    }

    auto invoice = *invoice_opt;

    if (invoice.paid) {
        throw std::runtime_error("Invoice already paid");
    }

    // Pay via Lightning through the configured backend bridge.
    auto ln_result = PayLightningInvoice(invoice.lightning_invoice);

    if (!ln_result["success"].asBool()) {
        std::string error = ln_result.get("error", Json::Value("Unknown error")).asString();
        throw std::runtime_error("Lightning payment failed: " + error);
    }

    // Mark invoice as paid in database
    std::string preimage = ln_result["payment_preimage"].asString();
    uint64_t paid_at = std::time(nullptr);

    if (!db_->MarkInvoicePaid(pay_period, employee_id_hash, preimage, paid_at)) {
        dinero::g_logger.warning("[PayrollRPC] Failed to mark invoice paid in database");
    }

    dinero::g_logger.info("[PayrollRPC] Invoice paid: " + employee_id_hash);

    din::Json result;
    result["success"] = true;
    result["payment_preimage"] = preimage;
    result["paid_at"] = Json::Value::UInt64(paid_at);
    result["fee_paid"] = ln_result.get("fee_paid", Json::Value(100)).asUInt();

    return result;
}

din::Json PayrollRPC::PayAll(const ExecutionContext& ctx, const din::Json& params) {
    std::string pay_period = params.get("pay_period", Json::Value(GetCurrentPayPeriod())).asString();

    dinero::g_logger.info("[PayrollRPC] Batch paying all invoices for period: " + pay_period);

    // Fetch all unpaid invoices
    auto unpaid_invoices = db_->GetUnpaidInvoices(pay_period);

    dinero::g_logger.info("[PayrollRPC] Found " + std::to_string(unpaid_invoices.size()) + " unpaid invoices");

    din::Json results(Json::arrayValue);
    uint32_t paid_count = 0;
    uint32_t failed_count = 0;
    uint64_t total_amount = 0;
    uint64_t total_fees = 0;

    // Pay each invoice
    for (const auto& invoice : unpaid_invoices) {
        din::Json pay_params;
        pay_params["employee_id_hash"] = invoice.employee_id_hash;
        pay_params["pay_period"] = invoice.pay_period;

        try {
            auto pay_result = PayInvoice(ctx, pay_params);

            if (pay_result["success"].asBool()) {
                paid_count++;
                total_fees += pay_result.get("fee_paid", Json::Value(0)).asUInt();

                din::Json entry;
                entry["employee_id_hash"] = invoice.employee_id_hash;
                entry["success"] = true;
                entry["paid_at"] = pay_result["paid_at"];
                results.append(entry);
            } else {
                failed_count++;
            }

        } catch (const std::exception& e) {
            failed_count++;
            din::Json error_entry;
            error_entry["employee_id_hash"] = invoice.employee_id_hash;
            error_entry["success"] = false;
            error_entry["error"] = e.what();
            results.append(error_entry);
        }
    }

    din::Json result;
    result["pay_period"] = pay_period;
    result["total_invoices"] = static_cast<Json::UInt>(unpaid_invoices.size());
    result["paid_invoices"] = paid_count;
    result["failed_invoices"] = failed_count;
    result["total_amount"] = total_amount / 100000000.0;
    result["total_fees"] = Json::Value::UInt64(total_fees);
    result["results"] = results;

    dinero::g_logger.info("[PayrollRPC] Batch payment complete: " +
                          std::to_string(paid_count) + "/" + std::to_string(unpaid_invoices.size()) + " paid");

    return result;
}

din::Json PayrollRPC::Report(const ExecutionContext& ctx, const din::Json& params) {
    std::string pay_period = params.get("pay_period", Json::Value(GetCurrentPayPeriod())).asString();

    dinero::g_logger.info("[PayrollRPC] Generating report for period: " + pay_period);

    // Fetch all invoices for period
    auto all_invoices = db_->GetInvoices(pay_period);

    uint32_t total_employees = static_cast<uint32_t>(all_invoices.size());
    uint32_t paid_employees = db_->GetPaidCount(pay_period);
    uint32_t unpaid_employees = total_employees - paid_employees;

    din::Json entries(Json::arrayValue);

    for (const auto& invoice : all_invoices) {
        din::Json entry;
        entry["employee_id_hash"] = invoice.employee_id_hash;
        entry["pay_period"] = invoice.pay_period;
        entry["paid"] = invoice.paid;
        entry["paid_at"] = Json::Value::UInt64(invoice.paid_at);
        entry["payment_hash"] = invoice.payment_hash;
        entries.append(entry);
    }

    din::Json result;
    result["pay_period"] = pay_period;
    result["total_employees"] = total_employees;
    result["paid_employees"] = paid_employees;
    result["unpaid_employees"] = unpaid_employees;
    result["entries"] = entries;

    return result;
}

din::Json PayrollRPC::ListInvoices(const ExecutionContext& ctx, const din::Json& params) {
    std::string pay_period = params.get("pay_period", Json::Value("")).asString();
    std::string status = params.get("status", Json::Value("all")).asString();

    std::vector<dinero::payroll::PayrollInvoice> invoices;

    if (!pay_period.empty()) {
        if (status == "unpaid") {
            invoices = db_->GetUnpaidInvoices(pay_period);
        } else {
            invoices = db_->GetInvoices(pay_period);
        }
    }

    din::Json invoice_list(Json::arrayValue);

    for (const auto& inv : invoices) {
        din::Json entry;
        entry["employee_id_hash"] = inv.employee_id_hash;
        entry["pay_period"] = inv.pay_period;
        entry["lightning_invoice"] = inv.lightning_invoice;
        entry["payment_hash"] = inv.payment_hash;
        entry["paid"] = inv.paid;
        entry["timestamp"] = Json::Value::UInt64(inv.timestamp);
        invoice_list.append(entry);
    }

    din::Json result;
    result["count"] = static_cast<Json::UInt>(invoices.size());
    result["invoices"] = invoice_list;

    return result;
}

// ============================================================================
// Helper Methods
// ============================================================================

std::string PayrollRPC::GenerateInvoiceID() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream oss;
    oss << "inv_" << std::hex << ms;
    return oss.str();
}

std::string PayrollRPC::GetCurrentPayPeriod() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time_t);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << "-"
        << std::setfill('0') << std::setw(2) << (tm.tm_mon + 1);
    return oss.str();
}

std::string PayrollRPC::HashEmployeeID(const std::string& employee_id) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(employee_id.c_str()),
           employee_id.length(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

din::Json PayrollRPC::CreateLightningInvoice(uint64_t amount_una, const std::string& memo) {
    (void)amount_una;
    (void)memo;
    throw std::runtime_error(
        "payroll.createinvoice unavailable: Lightning invoice backend bridge is not configured");
}

din::Json PayrollRPC::PayLightningInvoice(const std::string& ln_invoice) {
    (void)ln_invoice;
    throw std::runtime_error(
        "payroll.payinvoice unavailable: Lightning payment backend bridge is not configured");
}

// ============================================================================
// Global Registration Function
// ============================================================================

void WirePayrollRpcContext(std::shared_ptr<dinero::payroll::PayrollDB> db) {
    auto payroll_rpc = std::make_shared<PayrollRPC>(db);
    payroll_rpc->Register();
}

} // namespace dinero::rpc
