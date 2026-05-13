#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace dinero {
namespace payroll {

/**
 * Payroll Invoice - represents a salary payment to an employee
 *
 * Phase 1 (MVP): Transparent amounts via Lightning
 * Phase 2: Add confidential commitments
 * Phase 3: Add aggregated proofs
 */
struct PayrollInvoice {
    // Unique identifier
    std::string invoice_id;  // Generated UUID

    // Employee information
    std::string employee_id;  // Can be hashed for privacy
    std::string employee_name;  // Optional, for display

    // Pay period
    std::string pay_period;  // Format: "YYYY-MM" (e.g., "2025-11")

    // Payment details (Phase 1: transparent)
    uint64_t gross_amount;  // Amount in una (una)
    std::string memo;  // Description (e.g., "November Salary")

    // Lightning invoice
    std::string ln_invoice;  // BOLT#11 invoice string
    std::string payment_hash;  // For tracking payment status

    // Payment status
    bool paid;
    int64_t paid_at;  // Unix timestamp (0 if unpaid)
    std::string payment_preimage;  // Proof of payment

    // Phase 2: Confidential fields (future)
    std::vector<uint8_t> commitment;  // Pedersen commitment (33 bytes)
    std::vector<uint8_t> range_proof;  // Bulletproof (~5KB)
    std::vector<uint8_t> view_nonce;  // For employee to extract amount (32 bytes)

    // Metadata
    int64_t created_at;  // Unix timestamp
    int64_t expires_at;  // Invoice expiration

    PayrollInvoice()
        : gross_amount(0), paid(false), paid_at(0), created_at(0), expires_at(0) {}
};

/**
 * Payroll Period Summary - aggregated payroll data for a period
 */
struct PayrollPeriodSummary {
    std::string pay_period;  // Format: "YYYY-MM"

    // Counts
    uint32_t total_employees;
    uint32_t paid_employees;
    uint32_t unpaid_employees;

    // Amounts (transparent in Phase 1)
    uint64_t total_gross;  // Total payroll amount
    uint64_t total_paid;  // Total actually paid
    uint64_t total_unpaid;  // Remaining to be paid

    // Phase 3: Aggregated proof (future)
    std::vector<uint8_t> total_commitment;  // Aggregate Pedersen commitment
    std::vector<uint8_t> payroll_proof;  // ZK proof that sum(employees) = total

    // Status
    bool finalized;  // True when payroll period is closed
    int64_t finalized_at;  // Unix timestamp

    PayrollPeriodSummary()
        : total_employees(0), paid_employees(0), unpaid_employees(0),
          total_gross(0), total_paid(0), total_unpaid(0),
          finalized(false), finalized_at(0) {}
};

/**
 * Payroll Payment Result - result of paying an invoice
 */
struct PayrollPaymentResult {
    std::string invoice_id;
    bool success;
    std::string error_message;  // If success = false
    std::string payment_preimage;  // Proof of payment
    int64_t paid_at;  // Unix timestamp
    uint64_t fee_paid;  // Lightning routing fee (in una)
};

/**
 * Payroll Report Entry - single line in a payroll report
 */
struct PayrollReportEntry {
    std::string employee_id;
    std::string employee_name;
    uint64_t gross_amount;
    bool paid;
    int64_t paid_at;
    std::string payment_hash;

    // Phase 2: View key for this employee (optional)
    std::vector<uint8_t> view_key;
};

/**
 * Payroll Configuration - employer settings
 */
struct PayrollConfig {
    // Default invoice expiration (seconds)
    uint32_t default_expiry;  // Default: 86400 (24 hours)

    // Automatic payment settings
    bool auto_pay_enabled;  // Automatically pay invoices when created
    uint64_t auto_pay_threshold;  // Only auto-pay if amount < threshold

    // Privacy settings (Phase 2+)
    bool confidential_by_default;  // Create confidential invoices
    bool aggregate_proofs_enabled;  // Generate period-level ZK proofs

    PayrollConfig()
        : default_expiry(86400), auto_pay_enabled(false), auto_pay_threshold(0),
          confidential_by_default(false), aggregate_proofs_enabled(false) {}
};

/**
 * Employee Record - basic employee information
 * Future: Could extend with zk-ID credentials
 */
struct EmployeeRecord {
    std::string employee_id;  // Internal ID (can be UUID)
    std::string employee_id_hash;  // SHA256 of actual employee ID (for privacy)
    std::string display_name;  // How to display in UI

    // Contact (optional, for sending view keys)
    std::string email;
    std::string notification_url;  // Webhook for payment notifications

    // Status
    bool active;
    int64_t hired_date;
    int64_t terminated_date;  // 0 if still employed

    // Phase 2: View key management
    std::vector<uint8_t> master_view_key;  // Derived from HD wallet

    EmployeeRecord()
        : active(true), hired_date(0), terminated_date(0) {}
};

} // namespace payroll
} // namespace dinero
