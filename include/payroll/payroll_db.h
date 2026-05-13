#pragma once

#include "payroll/payroll_types.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

// Forward declare SQLite connection
struct sqlite3;

namespace dinero {
namespace payroll {

/**
 * PayrollDB - SQLite database for payroll invoice storage
 *
 * Schema:
 * - payroll_invoices: Individual invoices
 * - payroll_periods: Period-level summaries
 * - payroll_employees: Employee records
 * - payroll_config: Configuration settings
 */
class PayrollDB {
public:
    /**
     * Constructor
     * @param db_path Path to SQLite database file
     */
    explicit PayrollDB(const std::string& db_path);

    /**
     * Destructor - closes database connection
     */
    ~PayrollDB();

    // Disable copy
    PayrollDB(const PayrollDB&) = delete;
    PayrollDB& operator=(const PayrollDB&) = delete;

    /**
     * Initialize database schema
     * Creates tables if they don't exist
     */
    bool initialize();

    // ========================================================================
    // Invoice Operations
    // ========================================================================

    /**
     * Insert a new payroll invoice
     */
    bool insertInvoice(const PayrollInvoice& invoice);

    /**
     * Get invoice by ID
     */
    std::optional<PayrollInvoice> getInvoice(const std::string& invoice_id);

    /**
     * Get invoice by payment hash (for Lightning payment tracking)
     */
    std::optional<PayrollInvoice> getInvoiceByPaymentHash(const std::string& payment_hash);

    /**
     * Get all invoices for an employee
     */
    std::vector<PayrollInvoice> getEmployeeInvoices(const std::string& employee_id);

    /**
     * Get all invoices for a pay period
     */
    std::vector<PayrollInvoice> getPeriodInvoices(const std::string& pay_period);

    /**
     * Get all unpaid invoices for a pay period
     */
    std::vector<PayrollInvoice> getUnpaidInvoices(const std::string& pay_period);

    /**
     * Update invoice payment status
     */
    bool markInvoicePaid(const std::string& invoice_id,
                         const std::string& payment_preimage,
                         int64_t paid_at);

    /**
     * Delete an invoice (if unpaid)
     */
    bool deleteInvoice(const std::string& invoice_id);

    // ========================================================================
    // Period Operations
    // ========================================================================

    /**
     * Get period summary (creates if doesn't exist)
     */
    PayrollPeriodSummary getPeriodSummary(const std::string& pay_period);

    /**
     * Update period summary (recalculates from invoices)
     */
    bool updatePeriodSummary(const std::string& pay_period);

    /**
     * Finalize a pay period (prevents further changes)
     */
    bool finalizePeriod(const std::string& pay_period);

    /**
     * Get all pay periods
     */
    std::vector<std::string> getAllPeriods();

    // ========================================================================
    // Employee Operations
    // ========================================================================

    /**
     * Insert or update employee record
     */
    bool upsertEmployee(const EmployeeRecord& employee);

    /**
     * Get employee by ID
     */
    std::optional<EmployeeRecord> getEmployee(const std::string& employee_id);

    /**
     * Get all active employees
     */
    std::vector<EmployeeRecord> getActiveEmployees();

    /**
     * Deactivate employee
     */
    bool deactivateEmployee(const std::string& employee_id);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Get payroll configuration
     */
    PayrollConfig getConfig();

    /**
     * Update payroll configuration
     */
    bool updateConfig(const PayrollConfig& config);

    // ========================================================================
    // Reporting
    // ========================================================================

    /**
     * Generate payroll report for a period
     */
    std::vector<PayrollReportEntry> generateReport(const std::string& pay_period);

    /**
     * Get total payroll for a period
     */
    uint64_t getTotalPayroll(const std::string& pay_period);

    /**
     * Get employee YTD (year-to-date) total
     */
    uint64_t getEmployeeYTD(const std::string& employee_id, int year);

private:
    sqlite3* db_;
    std::string db_path_;

    // Helper: Execute SQL statement
    bool execute(const std::string& sql);

    // Helper: Serialize/deserialize BLOBs
    static std::vector<uint8_t> blobToVector(const void* blob, int size);
    static const void* vectorToBlob(const std::vector<uint8_t>& vec);
};

} // namespace payroll
} // namespace dinero
