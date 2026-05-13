#pragma once

#include <rocksdb/db.h>
#include <json/json.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace dinero::payroll {

/**
 * PayrollInvoice - Represents a salary payment to an employee
 * Stored in RocksDB with JSON serialization
 */
struct PayrollInvoice {
    std::string employee_id_hash;       // SHA256 hash of employee ID
    std::string pay_period;             // Format: "YYYY-MM"
    std::string lightning_invoice;      // BOLT#11 invoice string
    std::string payment_hash;           // For tracking payment
    std::string gross_commitment;       // hex-encoded Pedersen commitment (Phase 2)
    std::string range_proof;            // hex-encoded Bulletproof (Phase 2)
    std::string view_nonce;             // hex-encoded nonce for employee (Phase 2)
    uint64_t timestamp;                 // Creation timestamp
    bool paid;                          // Payment status
    std::string payment_preimage;       // Proof of payment (if paid)
    uint64_t paid_at;                   // Payment timestamp

    Json::Value ToJSON() const;
    static PayrollInvoice FromJSON(const Json::Value& v);
};

/**
 * PayrollProof - Aggregated ZK proof for a pay period (Phase 3)
 */
struct PayrollProof {
    std::string pay_period;             // Format: "YYYY-MM"
    std::string aggregated_commitment;  // hex-encoded aggregate commitment
    std::string aggregated_range_proof; // hex-encoded aggregate proof
    std::string audit_hash;             // Hash for verification
    uint64_t timestamp;                 // Proof generation time
    uint32_t employee_count;            // Number of employees in period

    Json::Value ToJSON() const;
    static PayrollProof FromJSON(const Json::Value& v);
};

/**
 * PayrollDB - RocksDB-based storage for payroll data
 *
 * Key format:
 *   - Invoices: "invoice:{pay_period}:{employee_hash}"
 *   - Proofs:   "proof:{pay_period}"
 *   - Config:   "config:default"
 */
class PayrollDB {
public:
    explicit PayrollDB(const std::string& path);
    ~PayrollDB();

    // Initialization
    bool Open();
    void Close();

    // Invoice operations
    bool StoreInvoice(const PayrollInvoice& inv);
    std::optional<PayrollInvoice> GetInvoice(const std::string& period,
                                             const std::string& emp_hash);
    std::vector<PayrollInvoice> GetInvoices(const std::string& period);
    std::vector<PayrollInvoice> GetUnpaidInvoices(const std::string& period);
    bool MarkInvoicePaid(const std::string& period,
                         const std::string& emp_hash,
                         const std::string& preimage,
                         uint64_t paid_at);

    // Proof operations (Phase 3)
    bool StoreProof(const PayrollProof& proof);
    std::optional<PayrollProof> GetProof(const std::string& period);
    std::vector<std::string> GetAllPeriods();

    // Statistics
    uint32_t GetEmployeeCount(const std::string& period);
    uint32_t GetPaidCount(const std::string& period);

private:
    std::unique_ptr<rocksdb::DB> db_;
    std::string db_path_;

    // Key construction helpers
    std::string MakeInvoiceKey(const std::string& period,
                               const std::string& emp_hash) const;
    std::string MakeProofKey(const std::string& period) const;
    std::string MakeInvoicePrefix(const std::string& period) const;
};

} // namespace dinero::payroll
