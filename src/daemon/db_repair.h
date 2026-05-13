// db_repair.h - Database integrity check and repair utility
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace repair {

/**
 * Database issue types detected during scan
 */
enum class IssueType {
    NullPath,           // NULL value in path column
    EmptyPath,          // Empty string in path column
    InvalidPath,        // Path contains invalid characters or format
    OrphanedRecord,     // Record references non-existent parent
    InvalidHeight,      // Height value is negative or impossibly large
    NullAddress,        // NULL address in addresses table
    EmptyAddress,       // Empty string address
    InvalidWalletId,    // References non-existent wallet
    CorruptBlob,        // Binary data that can't be decoded
};

/**
 * Represents a single detected issue
 */
struct DatabaseIssue {
    IssueType type;
    std::string table_name;
    std::string column_name;
    int64_t row_id;
    std::string description;
    bool repairable;      // Can this be safely repaired?

    std::string type_string() const;
};

/**
 * Result of a database repair scan
 */
struct RepairReport {
    std::string database_path;
    uint64_t tables_scanned;
    uint64_t rows_scanned;
    std::vector<DatabaseIssue> issues;
    bool has_critical_issues;

    uint64_t repairable_count() const;
    uint64_t critical_count() const;
    void print_summary() const;
    void print_detailed() const;
};

/**
 * Database repair utility
 *
 * Usage:
 *   1. scan() - Read-only scan, returns report of issues
 *   2. repair() - Fix repairable issues (requires explicit confirmation)
 */
class DatabaseRepair {
public:
    explicit DatabaseRepair(const std::string& datadir);
    ~DatabaseRepair();

    /**
     * Scan all databases for issues (read-only)
     * @return Report containing all detected issues
     */
    RepairReport scan();

    /**
     * Repair detected issues
     * @param report Previous scan report
     * @param dry_run If true, only simulate repairs
     * @return Number of issues fixed
     */
    uint64_t repair(const RepairReport& report, bool dry_run = false);

private:
    std::string datadir_;

    // Scan specific databases
    void scan_wallet_registry(RepairReport& report);
    void scan_wallet_databases(RepairReport& report);
    void scan_utxo_index(RepairReport& report);

    // Repair helpers
    bool delete_row(const std::string& db_path, const std::string& table, int64_t row_id);
    bool update_null_to_empty(const std::string& db_path, const std::string& table,
                              const std::string& column, int64_t row_id);
};

/**
 * Main entry point for --repair-db command
 * @param datadir Data directory path
 * @param confirm If true, perform repairs; if false, report only
 * @return 0 on success, non-zero on error
 */
int run_repair_db(const std::string& datadir, bool confirm);

} // namespace repair
} // namespace dinero
