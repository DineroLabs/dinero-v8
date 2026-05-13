#include "database/payroll_db.h"
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <sstream>
#include <iostream>

namespace dinero::payroll {

// ============================================================================
// PayrollInvoice Serialization
// ============================================================================

Json::Value PayrollInvoice::ToJSON() const {
    Json::Value v;
    v["employee_id_hash"] = employee_id_hash;
    v["pay_period"] = pay_period;
    v["lightning_invoice"] = lightning_invoice;
    v["payment_hash"] = payment_hash;
    v["gross_commitment"] = gross_commitment;
    v["range_proof"] = range_proof;
    v["view_nonce"] = view_nonce;
    v["timestamp"] = Json::Value::UInt64(timestamp);
    v["paid"] = paid;
    v["payment_preimage"] = payment_preimage;
    v["paid_at"] = Json::Value::UInt64(paid_at);
    return v;
}

PayrollInvoice PayrollInvoice::FromJSON(const Json::Value& v) {
    PayrollInvoice inv;
    inv.employee_id_hash = v["employee_id_hash"].asString();
    inv.pay_period = v["pay_period"].asString();
    inv.lightning_invoice = v["lightning_invoice"].asString();
    inv.payment_hash = v["payment_hash"].asString();
    inv.gross_commitment = v["gross_commitment"].asString();
    inv.range_proof = v["range_proof"].asString();
    inv.view_nonce = v["view_nonce"].asString();
    inv.timestamp = v["timestamp"].asUInt64();
    inv.paid = v["paid"].asBool();
    inv.payment_preimage = v["payment_preimage"].asString();
    inv.paid_at = v["paid_at"].asUInt64();
    return inv;
}

// ============================================================================
// PayrollProof Serialization
// ============================================================================

Json::Value PayrollProof::ToJSON() const {
    Json::Value v;
    v["pay_period"] = pay_period;
    v["aggregated_commitment"] = aggregated_commitment;
    v["aggregated_range_proof"] = aggregated_range_proof;
    v["audit_hash"] = audit_hash;
    v["timestamp"] = Json::Value::UInt64(timestamp);
    v["employee_count"] = employee_count;
    return v;
}

PayrollProof PayrollProof::FromJSON(const Json::Value& v) {
    PayrollProof proof;
    proof.pay_period = v["pay_period"].asString();
    proof.aggregated_commitment = v["aggregated_commitment"].asString();
    proof.aggregated_range_proof = v["aggregated_range_proof"].asString();
    proof.audit_hash = v["audit_hash"].asString();
    proof.timestamp = v["timestamp"].asUInt64();
    proof.employee_count = v["employee_count"].asUInt();
    return proof;
}

// ============================================================================
// PayrollDB Implementation
// ============================================================================

PayrollDB::PayrollDB(const std::string& path)
    : db_(nullptr), db_path_(path) {
}

PayrollDB::~PayrollDB() {
    Close();
}

bool PayrollDB::Open() {
    if (db_) {
        return true;  // Already open
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;

    rocksdb::DB* db_ptr = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, db_path_, &db_ptr);

    if (!status.ok()) {
        std::cerr << "[PayrollDB] Failed to open database: " << status.ToString() << std::endl;
        return false;
    }

    db_.reset(db_ptr);
    std::cout << "[PayrollDB] Database opened: " << db_path_ << std::endl;
    return true;
}

void PayrollDB::Close() {
    if (db_) {
        db_.reset();
        std::cout << "[PayrollDB] Database closed" << std::endl;
    }
}

// ============================================================================
// Invoice Operations
// ============================================================================

bool PayrollDB::StoreInvoice(const PayrollInvoice& inv) {
    if (!db_) {
        std::cerr << "[PayrollDB] Database not open" << std::endl;
        return false;
    }

    // Serialize to JSON
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";  // Compact JSON
    std::string data = Json::writeString(writer, inv.ToJSON());

    // Store in RocksDB
    auto key = MakeInvoiceKey(inv.pay_period, inv.employee_id_hash);
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, data);

    if (!status.ok()) {
        std::cerr << "[PayrollDB] Failed to store invoice: " << status.ToString() << std::endl;
        return false;
    }

    std::cout << "[PayrollDB] Stored invoice: " << key << std::endl;
    return true;
}

std::optional<PayrollInvoice> PayrollDB::GetInvoice(const std::string& period,
                                                     const std::string& emp_hash) {
    if (!db_) {
        return std::nullopt;
    }

    auto key = MakeInvoiceKey(period, emp_hash);
    std::string value;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);

    if (!status.ok()) {
        if (!status.IsNotFound()) {
            std::cerr << "[PayrollDB] Error reading invoice: " << status.ToString() << std::endl;
        }
        return std::nullopt;
    }

    // Deserialize JSON
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream ss(value);

    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        std::cerr << "[PayrollDB] Failed to parse JSON: " << errs << std::endl;
        return std::nullopt;
    }

    return PayrollInvoice::FromJSON(root);
}

std::vector<PayrollInvoice> PayrollDB::GetInvoices(const std::string& period) {
    std::vector<PayrollInvoice> results;
    if (!db_) {
        return results;
    }

    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    std::string prefix = MakeInvoicePrefix(period);

    for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string errs;
        std::string value = it->value().ToString();
        std::istringstream ss(value);

        if (Json::parseFromStream(reader, ss, &root, &errs)) {
            results.push_back(PayrollInvoice::FromJSON(root));
        } else {
            std::cerr << "[PayrollDB] Failed to parse invoice JSON: " << errs << std::endl;
        }
    }

    std::cout << "[PayrollDB] Retrieved " << results.size() << " invoices for period " << period << std::endl;
    return results;
}

std::vector<PayrollInvoice> PayrollDB::GetUnpaidInvoices(const std::string& period) {
    auto all_invoices = GetInvoices(period);
    std::vector<PayrollInvoice> unpaid;

    for (const auto& inv : all_invoices) {
        if (!inv.paid) {
            unpaid.push_back(inv);
        }
    }

    std::cout << "[PayrollDB] Found " << unpaid.size() << " unpaid invoices for period " << period << std::endl;
    return unpaid;
}

bool PayrollDB::MarkInvoicePaid(const std::string& period,
                                const std::string& emp_hash,
                                const std::string& preimage,
                                uint64_t paid_at) {
    // Fetch existing invoice
    auto inv_opt = GetInvoice(period, emp_hash);
    if (!inv_opt) {
        std::cerr << "[PayrollDB] Invoice not found for update" << std::endl;
        return false;
    }

    // Update payment fields
    PayrollInvoice inv = *inv_opt;
    inv.paid = true;
    inv.payment_preimage = preimage;
    inv.paid_at = paid_at;

    // Store back
    return StoreInvoice(inv);
}

// ============================================================================
// Proof Operations (Phase 3)
// ============================================================================

bool PayrollDB::StoreProof(const PayrollProof& proof) {
    if (!db_) {
        return false;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string data = Json::writeString(writer, proof.ToJSON());

    auto key = MakeProofKey(proof.pay_period);
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, data);

    if (!status.ok()) {
        std::cerr << "[PayrollDB] Failed to store proof: " << status.ToString() << std::endl;
        return false;
    }

    std::cout << "[PayrollDB] Stored proof: " << key << std::endl;
    return true;
}

std::optional<PayrollProof> PayrollDB::GetProof(const std::string& period) {
    if (!db_) {
        return std::nullopt;
    }

    auto key = MakeProofKey(period);
    std::string value;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);

    if (!status.ok()) {
        return std::nullopt;
    }

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream ss(value);

    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        return std::nullopt;
    }

    return PayrollProof::FromJSON(root);
}

std::vector<std::string> PayrollDB::GetAllPeriods() {
    std::vector<std::string> periods;
    if (!db_) {
        return periods;
    }

    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    std::string prefix = "invoice:";
    std::string last_period;

    for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
        std::string key = it->key().ToString();
        // Key format: "invoice:YYYY-MM:emp_hash"
        size_t first_colon = key.find(':');
        size_t second_colon = key.find(':', first_colon + 1);

        if (second_colon != std::string::npos) {
            std::string period = key.substr(first_colon + 1, second_colon - first_colon - 1);
            if (period != last_period) {
                periods.push_back(period);
                last_period = period;
            }
        }
    }

    return periods;
}

// ============================================================================
// Statistics
// ============================================================================

uint32_t PayrollDB::GetEmployeeCount(const std::string& period) {
    return static_cast<uint32_t>(GetInvoices(period).size());
}

uint32_t PayrollDB::GetPaidCount(const std::string& period) {
    auto all_invoices = GetInvoices(period);
    uint32_t paid_count = 0;

    for (const auto& inv : all_invoices) {
        if (inv.paid) {
            paid_count++;
        }
    }

    return paid_count;
}

// ============================================================================
// Key Construction Helpers
// ============================================================================

std::string PayrollDB::MakeInvoiceKey(const std::string& period,
                                      const std::string& emp_hash) const {
    return "invoice:" + period + ":" + emp_hash;
}

std::string PayrollDB::MakeProofKey(const std::string& period) const {
    return "proof:" + period;
}

std::string PayrollDB::MakeInvoicePrefix(const std::string& period) const {
    return "invoice:" + period + ":";
}

} // namespace dinero::payroll
