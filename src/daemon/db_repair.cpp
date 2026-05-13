// db_repair.cpp - Database integrity check and repair utility
#include "db_repair.h"

#include <iostream>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace dinero {
namespace repair {

// ═══════════════════════════════════════════════════════════════════
// DatabaseIssue implementation
// ═══════════════════════════════════════════════════════════════════

std::string DatabaseIssue::type_string() const {
    switch (type) {
        case IssueType::NullPath:       return "NULL_PATH";
        case IssueType::EmptyPath:      return "EMPTY_PATH";
        case IssueType::InvalidPath:    return "INVALID_PATH";
        case IssueType::OrphanedRecord: return "ORPHANED_RECORD";
        case IssueType::InvalidHeight:  return "INVALID_HEIGHT";
        case IssueType::NullAddress:    return "NULL_ADDRESS";
        case IssueType::EmptyAddress:   return "EMPTY_ADDRESS";
        case IssueType::InvalidWalletId: return "INVALID_WALLET_ID";
        case IssueType::CorruptBlob:    return "CORRUPT_BLOB";
        default:                        return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════
// RepairReport implementation
// ═══════════════════════════════════════════════════════════════════

uint64_t RepairReport::repairable_count() const {
    uint64_t count = 0;
    for (const auto& issue : issues) {
        if (issue.repairable) count++;
    }
    return count;
}

uint64_t RepairReport::critical_count() const {
    uint64_t count = 0;
    for (const auto& issue : issues) {
        if (!issue.repairable) count++;
    }
    return count;
}

void RepairReport::print_summary() const {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << "                    DATABASE REPAIR REPORT\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";

    std::cout << "📁 Data directory: " << database_path << "\n";
    std::cout << "📊 Tables scanned: " << tables_scanned << "\n";
    std::cout << "📝 Rows scanned:   " << rows_scanned << "\n";
    std::cout << "\n";

    if (issues.empty()) {
        std::cout << "✅ No issues detected. Database is healthy.\n";
    } else {
        std::cout << "⚠️  Issues detected: " << issues.size() << "\n";
        std::cout << "   ├── Repairable:  " << repairable_count() << "\n";
        std::cout << "   └── Critical:    " << critical_count() << "\n";
        std::cout << "\n";

        if (has_critical_issues) {
            std::cout << "🚨 CRITICAL: Some issues cannot be automatically repaired.\n";
            std::cout << "   Consider starting with a fresh data directory.\n";
        }
    }

    std::cout << "\n═══════════════════════════════════════════════════════════════════\n";
}

void RepairReport::print_detailed() const {
    print_summary();

    if (issues.empty()) return;

    std::cout << "\nDetailed Issues:\n";
    std::cout << "───────────────────────────────────────────────────────────────────\n";

    int idx = 1;
    for (const auto& issue : issues) {
        std::cout << "\n[" << idx++ << "] " << issue.type_string() << "\n";
        std::cout << "    Table:    " << issue.table_name << "\n";
        std::cout << "    Column:   " << issue.column_name << "\n";
        std::cout << "    Row ID:   " << issue.row_id << "\n";
        std::cout << "    Details:  " << issue.description << "\n";
        std::cout << "    Status:   " << (issue.repairable ? "✅ Repairable" : "❌ Manual fix required") << "\n";
    }

    std::cout << "\n───────────────────────────────────────────────────────────────────\n";

    if (repairable_count() > 0) {
        std::cout << "\nTo repair, run with --repair-db --confirm\n";
    }
}

// ═══════════════════════════════════════════════════════════════════
// DatabaseRepair implementation
// ═══════════════════════════════════════════════════════════════════

DatabaseRepair::DatabaseRepair(const std::string& datadir)
    : datadir_(datadir) {
}

DatabaseRepair::~DatabaseRepair() = default;

RepairReport DatabaseRepair::scan() {
    RepairReport report;
    report.database_path = datadir_;
    report.tables_scanned = 0;
    report.rows_scanned = 0;
    report.has_critical_issues = false;

    std::cout << "🔍 Scanning databases in: " << datadir_ << "\n";

    // Scan wallet registry
    scan_wallet_registry(report);

    // Scan individual wallet databases
    scan_wallet_databases(report);

    // Scan UTXO index
    scan_utxo_index(report);

    // Check for critical issues
    for (const auto& issue : report.issues) {
        if (!issue.repairable) {
            report.has_critical_issues = true;
            break;
        }
    }

    return report;
}

void DatabaseRepair::scan_wallet_registry(RepairReport& report) {
    std::string registry_path = datadir_ + "/wallet_registry.db";

    if (!fs::exists(registry_path)) {
        std::cout << "   ℹ️  No wallet registry found (OK for fresh install)\n";
        return;
    }

    std::cout << "   📋 Scanning wallet_registry.db...\n";

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(registry_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "   ❌ Failed to open wallet registry: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return;
    }

    // Check wallets table for NULL/empty paths
    const char* sql = "SELECT id, name, path FROM wallets";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

    if (rc == SQLITE_OK) {
        report.tables_scanned++;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            report.rows_scanned++;

            int64_t id = sqlite3_column_int64(stmt, 0);
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            const char* path = (const char*)sqlite3_column_text(stmt, 2);

            // Check for NULL name
            if (name == nullptr) {
                DatabaseIssue issue;
                issue.type = IssueType::NullPath;
                issue.table_name = "wallets";
                issue.column_name = "name";
                issue.row_id = id;
                issue.description = "Wallet has NULL name";
                issue.repairable = true;  // Can delete orphaned wallet entry
                report.issues.push_back(issue);
            } else if (strlen(name) == 0) {
                DatabaseIssue issue;
                issue.type = IssueType::EmptyPath;
                issue.table_name = "wallets";
                issue.column_name = "name";
                issue.row_id = id;
                issue.description = "Wallet has empty name";
                issue.repairable = true;
                report.issues.push_back(issue);
            }

            // Check for NULL path
            if (path == nullptr) {
                DatabaseIssue issue;
                issue.type = IssueType::NullPath;
                issue.table_name = "wallets";
                issue.column_name = "path";
                issue.row_id = id;
                issue.description = "Wallet entry has NULL path - DANGEROUS";
                issue.repairable = true;
                report.issues.push_back(issue);
            } else if (strlen(path) == 0) {
                DatabaseIssue issue;
                issue.type = IssueType::EmptyPath;
                issue.table_name = "wallets";
                issue.column_name = "path";
                issue.row_id = id;
                issue.description = "Wallet entry has empty path - DANGEROUS";
                issue.repairable = true;
                report.issues.push_back(issue);
            } else if (path[0] != '/' && path[1] != ':') {
                // Check for relative paths (should be absolute)
                DatabaseIssue issue;
                issue.type = IssueType::InvalidPath;
                issue.table_name = "wallets";
                issue.column_name = "path";
                issue.row_id = id;
                issue.description = "Wallet path is not absolute: " + std::string(path);
                issue.repairable = false;  // Need manual intervention
                report.issues.push_back(issue);
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void DatabaseRepair::scan_wallet_databases(RepairReport& report) {
    // Find all wallet_*.db files
    std::string wallets_dir = datadir_;

    if (!fs::exists(wallets_dir)) {
        return;
    }

    for (const auto& entry : fs::directory_iterator(wallets_dir)) {
        std::string filename = entry.path().filename().string();
        if (filename.find("wallet_") == 0 && filename.find(".db") != std::string::npos) {
            std::string wallet_path = entry.path().string();
            std::cout << "   💼 Scanning " << filename << "...\n";

            sqlite3* db = nullptr;
            int rc = sqlite3_open_v2(wallet_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
            if (rc != SQLITE_OK) {
                std::cerr << "   ❌ Failed to open " << filename << ": " << sqlite3_errmsg(db) << "\n";
                sqlite3_close(db);
                continue;
            }

            // Check addresses table
            const char* addr_sql = "SELECT id, wallet_id, address, label FROM addresses";
            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2(db, addr_sql, -1, &stmt, nullptr);

            if (rc == SQLITE_OK) {
                report.tables_scanned++;

                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    report.rows_scanned++;

                    int64_t id = sqlite3_column_int64(stmt, 0);
                    int wallet_id = sqlite3_column_int(stmt, 1);
                    const char* address = (const char*)sqlite3_column_text(stmt, 2);

                    if (address == nullptr) {
                        DatabaseIssue issue;
                        issue.type = IssueType::NullAddress;
                        issue.table_name = "addresses";
                        issue.column_name = "address";
                        issue.row_id = id;
                        issue.description = "NULL address in wallet " + filename;
                        issue.repairable = true;  // Can delete invalid entry
                        report.issues.push_back(issue);
                    } else if (strlen(address) == 0) {
                        DatabaseIssue issue;
                        issue.type = IssueType::EmptyAddress;
                        issue.table_name = "addresses";
                        issue.column_name = "address";
                        issue.row_id = id;
                        issue.description = "Empty address string in wallet " + filename;
                        issue.repairable = true;
                        report.issues.push_back(issue);
                    }

                    // Check wallet_id is valid
                    if (wallet_id <= 0) {
                        DatabaseIssue issue;
                        issue.type = IssueType::InvalidWalletId;
                        issue.table_name = "addresses";
                        issue.column_name = "wallet_id";
                        issue.row_id = id;
                        issue.description = "Invalid wallet_id: " + std::to_string(wallet_id);
                        issue.repairable = true;
                        report.issues.push_back(issue);
                    }
                }
            }
            sqlite3_finalize(stmt);

            // Check UTXOs table if exists
            const char* utxo_sql = "SELECT id, txid, address, value FROM utxos";
            stmt = nullptr;
            rc = sqlite3_prepare_v2(db, utxo_sql, -1, &stmt, nullptr);

            if (rc == SQLITE_OK) {
                report.tables_scanned++;

                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    report.rows_scanned++;

                    int64_t id = sqlite3_column_int64(stmt, 0);
                    const char* txid = (const char*)sqlite3_column_text(stmt, 1);
                    const char* address = (const char*)sqlite3_column_text(stmt, 2);

                    if (txid == nullptr || strlen(txid) == 0) {
                        DatabaseIssue issue;
                        issue.type = IssueType::NullPath;
                        issue.table_name = "utxos";
                        issue.column_name = "txid";
                        issue.row_id = id;
                        issue.description = "NULL or empty txid in UTXO";
                        issue.repairable = true;
                        report.issues.push_back(issue);
                    }

                    if (address == nullptr || strlen(address) == 0) {
                        DatabaseIssue issue;
                        issue.type = IssueType::NullAddress;
                        issue.table_name = "utxos";
                        issue.column_name = "address";
                        issue.row_id = id;
                        issue.description = "NULL or empty address in UTXO";
                        issue.repairable = true;
                        report.issues.push_back(issue);
                    }
                }
            }
            sqlite3_finalize(stmt);

            sqlite3_close(db);
        }
    }
}

void DatabaseRepair::scan_utxo_index(RepairReport& report) {
    std::string utxo_path = datadir_ + "/blockchain/utxo";

    if (!fs::exists(utxo_path)) {
        std::cout << "   ℹ️  No UTXO index found (OK for fresh install)\n";
        return;
    }

    std::cout << "   🗃️  Checking UTXO index...\n";

    // UTXO index is SQLite, check for basic integrity
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(utxo_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "   ❌ Failed to open UTXO index: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);

        DatabaseIssue issue;
        issue.type = IssueType::CorruptBlob;
        issue.table_name = "utxo_index";
        issue.column_name = "database";
        issue.row_id = 0;
        issue.description = "UTXO index database is corrupted or inaccessible";
        issue.repairable = false;  // Need full reindex
        report.issues.push_back(issue);
        report.has_critical_issues = true;
        return;
    }

    // Run SQLite integrity check
    const char* integrity_sql = "PRAGMA integrity_check";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, integrity_sql, -1, &stmt, nullptr);

    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* result = (const char*)sqlite3_column_text(stmt, 0);
        if (result && strcmp(result, "ok") != 0) {
            DatabaseIssue issue;
            issue.type = IssueType::CorruptBlob;
            issue.table_name = "utxo_index";
            issue.column_name = "integrity";
            issue.row_id = 0;
            issue.description = "UTXO integrity check failed: " + std::string(result);
            issue.repairable = false;
            report.issues.push_back(issue);
            report.has_critical_issues = true;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

uint64_t DatabaseRepair::repair(const RepairReport& report, bool dry_run) {
    if (report.issues.empty()) {
        std::cout << "✅ No issues to repair.\n";
        return 0;
    }

    uint64_t fixed = 0;
    std::cout << "\n🔧 " << (dry_run ? "DRY RUN - " : "") << "Repairing " << report.repairable_count() << " issues...\n\n";

    for (const auto& issue : report.issues) {
        if (!issue.repairable) {
            std::cout << "   ⏭️  Skipping " << issue.type_string() << " (manual fix required)\n";
            continue;
        }

        std::string db_path;

        // Determine database path
        if (issue.table_name == "wallets") {
            db_path = datadir_ + "/wallet_registry.db";
        } else if (issue.table_name == "addresses" || issue.table_name == "utxos") {
            // Need to find the right wallet database
            // For now, assume description contains the filename
            db_path = datadir_ + "/" + issue.description.substr(issue.description.rfind("wallet_"));
            if (db_path.find(".db") == std::string::npos) {
                // Fallback: scan for the file
                for (const auto& entry : fs::directory_iterator(datadir_)) {
                    std::string fname = entry.path().filename().string();
                    if (fname.find("wallet_") == 0 && fname.find(".db") != std::string::npos) {
                        db_path = entry.path().string();
                        break;
                    }
                }
            }
        }

        if (dry_run) {
            std::cout << "   🔍 Would delete row " << issue.row_id << " from " << issue.table_name << "\n";
            fixed++;
        } else {
            if (delete_row(db_path, issue.table_name, issue.row_id)) {
                std::cout << "   ✅ Deleted row " << issue.row_id << " from " << issue.table_name << "\n";
                fixed++;
            } else {
                std::cout << "   ❌ Failed to delete row " << issue.row_id << " from " << issue.table_name << "\n";
            }
        }
    }

    std::cout << "\n" << (dry_run ? "Would fix: " : "Fixed: ") << fixed << "/" << report.repairable_count() << " issues\n";
    return fixed;
}

bool DatabaseRepair::delete_row(const std::string& db_path, const std::string& table, int64_t row_id) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    std::string sql = "DELETE FROM " + table + " WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, row_id);
    rc = sqlite3_step(stmt);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return rc == SQLITE_DONE;
}

bool DatabaseRepair::update_null_to_empty(const std::string& db_path, const std::string& table,
                                          const std::string& column, int64_t row_id) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    std::string sql = "UPDATE " + table + " SET " + column + " = '' WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, row_id);
    rc = sqlite3_step(stmt);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return rc == SQLITE_DONE;
}

// ═══════════════════════════════════════════════════════════════════
// Main entry point
// ═══════════════════════════════════════════════════════════════════

int run_repair_db(const std::string& datadir, bool confirm) {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           DINERO DATABASE REPAIR UTILITY                          ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════╝\n\n";

    if (!confirm) {
        std::cout << "Mode: READ-ONLY SCAN (no changes will be made)\n";
        std::cout << "      To repair, add --confirm flag\n\n";
    } else {
        std::cout << "Mode: REPAIR (will modify databases)\n";
        std::cout << "      ⚠️  Make sure you have backups!\n\n";
    }

    // Check datadir exists
    if (!fs::exists(datadir)) {
        std::cerr << "❌ Error: Data directory does not exist: " << datadir << "\n";
        return 1;
    }

    DatabaseRepair repair(datadir);

    // Phase 1: Scan
    RepairReport report = repair.scan();

    // Print report
    report.print_detailed();

    // Phase 2: Repair (if confirmed)
    if (confirm && report.repairable_count() > 0) {
        std::cout << "\n";
        std::cout << "═══════════════════════════════════════════════════════════════════\n";
        std::cout << "                    PERFORMING REPAIRS\n";
        std::cout << "═══════════════════════════════════════════════════════════════════\n";

        uint64_t fixed = repair.repair(report, false);

        if (fixed > 0) {
            std::cout << "\n✅ Repair complete. Fixed " << fixed << " issues.\n";
            std::cout << "   Restart the daemon to verify.\n";
        }
    } else if (confirm && report.repairable_count() == 0) {
        if (report.has_critical_issues) {
            std::cout << "\n⚠️  Critical issues detected that require manual intervention.\n";
            std::cout << "   Consider using --reindex or starting with a fresh data directory.\n";
            return 2;
        }
    }

    return report.has_critical_issues ? 2 : 0;
}

} // namespace repair
} // namespace dinero
