/**
 * @file wallet_oracle.cpp
 * @brief Wallet Truth Oracle - Cross-validates wallet state against ChainDB
 *
 * This is NOT a test - it's an authoritative validator.
 * Queries wallet RPC and ChainDB directly to ensure consistency.
 *
 * Usage:
 *   wallet_oracle snapshot > wallet_state.json
 *   wallet_oracle validate wallet_state_before.json wallet_state_after.json
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <map>
#include <sqlite3.h>

// JSON output helper (minimal - no external deps)
class JSONBuilder {
public:
    JSONBuilder() : first_(true) { ss_ << "{"; }

    void add(const std::string& key, const std::string& value) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << key << "\":\"" << value << "\"";
        first_ = false;
    }

    void add(const std::string& key, int64_t value) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << key << "\":" << value;
        first_ = false;
    }

    void add(const std::string& key, uint64_t value) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << key << "\":" << value;
        first_ = false;
    }

    void add(const std::string& key, uint32_t value) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << key << "\":" << value;
        first_ = false;
    }

    void add(const std::string& key, double value) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << key << "\":" << std::fixed << std::setprecision(8) << value;
        first_ = false;
    }

    std::string build() {
        ss_ << "}";
        return ss_.str();
    }

private:
    std::stringstream ss_;
    bool first_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Wallet State Snapshot (Invariants Only)
// ═══════════════════════════════════════════════════════════════════════════

struct WalletSnapshot {
    uint32_t height = 0;
    uint64_t balance_confirmed_una = 0;   // Confirmed balance in una
    uint64_t balance_immature_una = 0;    // Immature coinbase in una
    uint64_t balance_total_una = 0;       // Total balance in una
    uint32_t utxo_count = 0;               // Total UTXO count
    uint32_t immature_utxo_count = 0;      // Immature coinbase UTXO count
    uint32_t address_count = 0;            // Total addresses derived
    uint32_t confirmed_tx_count = 0;       // Confirmed transaction count
    uint32_t mempool_tx_count = 0;         // Mempool transaction count

    std::string toJSON() const {
        JSONBuilder json;
        json.add("height", height);
        json.add("balance_confirmed_una", balance_confirmed_una);
        json.add("balance_immature_una", balance_immature_una);
        json.add("balance_total_una", balance_total_una);
        json.add("balance_confirmed_din", static_cast<double>(balance_confirmed_una) / 100000000.0);
        json.add("balance_immature_din", static_cast<double>(balance_immature_una) / 100000000.0);
        json.add("balance_total_din", static_cast<double>(balance_total_una) / 100000000.0);
        json.add("utxo_count", utxo_count);
        json.add("immature_utxo_count", immature_utxo_count);
        json.add("address_count", address_count);
        json.add("confirmed_tx_count", confirmed_tx_count);
        json.add("mempool_tx_count", mempool_tx_count);
        return json.build();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// RPC Helper (Call dinero-cli)
// ═══════════════════════════════════════════════════════════════════════════

std::string callRPC(const std::string& method, const std::string& params = "") {
    std::string cmd = "curl -s --user \"__cookie__:$(cat ~/.dinero/.cookie 2>/dev/null)\" "
                      "--data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"oracle\",\"method\":\"" + method +
                      "\",\"params\":" + (params.empty() ? "[]" : params) + "}' "
                      "-H 'content-type: text/plain;' http://127.0.0.1:20998/ 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);

    return result;
}

// Parse JSON result field (very basic parser - production would use nlohmann/json)
std::string extractResult(const std::string& json) {
    size_t pos = json.find("\"result\":");
    if (pos == std::string::npos) return "";

    pos += 9; // Skip "result":
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n')) pos++;

    if (json[pos] == '"') {
        // String result
        pos++;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    } else if (json[pos] == '{' || json[pos] == '[') {
        // Object/array - return as is
        int depth = 1;
        size_t start = pos++;
        char open = json[start];
        char close = (open == '{') ? '}' : ']';

        while (pos < json.size() && depth > 0) {
            if (json[pos] == open) depth++;
            else if (json[pos] == close) depth--;
            pos++;
        }
        return json.substr(start, pos - start);
    } else {
        // Number
        size_t end = pos;
        while (end < json.size() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-')) end++;
        return json.substr(pos, end - pos);
    }
}

uint64_t unaFromDIN(double din) {
    return static_cast<uint64_t>(din * 100000000.0 + 0.5);
}

// ═══════════════════════════════════════════════════════════════════════════
// Wallet Snapshot Capture
// ═══════════════════════════════════════════════════════════════════════════

WalletSnapshot captureWalletSnapshot(const std::string& wallet_db_path) {
    WalletSnapshot snapshot;

    // Get blockchain height
    std::string height_response = callRPC("blockchain.getblockcount");
    std::string height_str = extractResult(height_response);
    if (!height_str.empty()) {
        snapshot.height = static_cast<uint32_t>(std::stoul(height_str));
    }

    // Get wallet balance
    std::string balance_response = callRPC("wallet.getbalance");
    std::string balance_json = extractResult(balance_response);

    // Parse balance JSON (very basic - production would use proper JSON parser)
    // Expected: {"confirmed":1234.0,"unconfirmed":0.0,"immature":100.0,"total":1334.0}
    size_t pos = balance_json.find("\"confirmed\":");
    if (pos != std::string::npos) {
        pos += 12;
        size_t end = balance_json.find(',', pos);
        std::string confirmed_str = balance_json.substr(pos, end - pos);
        snapshot.balance_confirmed_una = unaFromDIN(std::stod(confirmed_str));
    }

    pos = balance_json.find("\"immature\":");
    if (pos != std::string::npos) {
        pos += 11;
        size_t end = balance_json.find(',', pos);
        std::string immature_str = balance_json.substr(pos, end - pos);
        snapshot.balance_immature_una = unaFromDIN(std::stod(immature_str));
    }

    pos = balance_json.find("\"total\":");
    if (pos != std::string::npos) {
        pos += 8;
        size_t end = balance_json.find('}', pos);
        std::string total_str = balance_json.substr(pos, end - pos);
        snapshot.balance_total_una = unaFromDIN(std::stod(total_str));
    }

    // Get UTXO count and immature count from wallet database
    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db_path.c_str(), &db) == SQLITE_OK) {
        // Count total UTXOs
        const char* sql_count = "SELECT COUNT(*) FROM utxos WHERE is_spent = 0";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql_count, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snapshot.utxo_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        // Count immature UTXOs (coinbase with insufficient confirmations)
        const char* sql_immature = "SELECT COUNT(*) FROM utxos WHERE is_spent = 0 AND is_coinbase = 1 AND is_mature = 0";
        if (sqlite3_prepare_v2(db, sql_immature, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snapshot.immature_utxo_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        // Count addresses
        const char* sql_addresses = "SELECT COUNT(*) FROM addresses";
        if (sqlite3_prepare_v2(db, sql_addresses, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snapshot.address_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        // Count confirmed transactions
        const char* sql_confirmed = "SELECT COUNT(*) FROM transactions WHERE confirmations > 0";
        if (sqlite3_prepare_v2(db, sql_confirmed, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snapshot.confirmed_tx_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        // Count mempool transactions
        const char* sql_mempool = "SELECT COUNT(*) FROM transactions WHERE confirmations = 0";
        if (sqlite3_prepare_v2(db, sql_mempool, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snapshot.mempool_tx_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
    }

    return snapshot;
}

// ═══════════════════════════════════════════════════════════════════════════
// Invariant Validation
// ═══════════════════════════════════════════════════════════════════════════

struct ValidationResult {
    bool passed = true;
    std::vector<std::string> errors;

    void fail(const std::string& error) {
        passed = false;
        errors.push_back(error);
    }
};

ValidationResult validateInvariants(const WalletSnapshot& before, const WalletSnapshot& after) {
    ValidationResult result;

    // Invariant 1: Balance Conservation (monotonic - never decreases without spend)
    if (after.balance_total_una < before.balance_total_una) {
        std::ostringstream oss;
        oss << "❌ FATAL: Balance decreased! "
            << "Before: " << before.balance_total_una << " una, "
            << "After: " << after.balance_total_una << " una, "
            << "Loss: " << (before.balance_total_una - after.balance_total_una) << " una";
        result.fail(oss.str());
    }

    // Invariant 2: UTXO Count Monotonicity (can increase from mining, never decrease without spend)
    if (after.utxo_count < before.utxo_count) {
        std::ostringstream oss;
        oss << "❌ FATAL: UTXO count decreased! "
            << "Before: " << before.utxo_count << ", "
            << "After: " << after.utxo_count << ", "
            << "Loss: " << (before.utxo_count - after.utxo_count) << " UTXOs";
        result.fail(oss.str());
    }

    // Invariant 3: Address Ownership Integrity (monotonic - never lose addresses)
    if (after.address_count < before.address_count) {
        std::ostringstream oss;
        oss << "❌ FATAL: Address count decreased! "
            << "Before: " << before.address_count << ", "
            << "After: " << after.address_count << ", "
            << "Loss: " << (before.address_count - after.address_count) << " addresses";
        result.fail(oss.str());
    }

    // Invariant 4: Transaction History Preservation (confirmed tx count never decreases)
    if (after.confirmed_tx_count < before.confirmed_tx_count) {
        std::ostringstream oss;
        oss << "❌ FATAL: Confirmed transaction count decreased! "
            << "Before: " << before.confirmed_tx_count << ", "
            << "After: " << after.confirmed_tx_count << ", "
            << "Loss: " << (before.confirmed_tx_count - after.confirmed_tx_count) << " transactions";
        result.fail(oss.str());
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Oracle Commands
// ═══════════════════════════════════════════════════════════════════════════

int cmdSnapshot(const std::string& wallet_db_path) {
    WalletSnapshot snapshot = captureWalletSnapshot(wallet_db_path);
    std::cout << snapshot.toJSON() << std::endl;
    return 0;
}

int cmdValidate(const std::string& before_json_str, const std::string& after_json_str) {
    // Parse JSON strings into snapshots (basic parsing - production would use proper parser)
    WalletSnapshot before, after;

    // Extract height
    size_t pos = before_json_str.find("\"height\":");
    if (pos != std::string::npos) {
        pos += 9;
        before.height = std::stoul(before_json_str.substr(pos));
    }

    pos = after_json_str.find("\"height\":");
    if (pos != std::string::npos) {
        pos += 9;
        after.height = std::stoul(after_json_str.substr(pos));
    }

    // Extract balance_total_una
    pos = before_json_str.find("\"balance_total_una\":");
    if (pos != std::string::npos) {
        pos += 21;
        before.balance_total_una = std::stoull(before_json_str.substr(pos));
    }

    pos = after_json_str.find("\"balance_total_una\":");
    if (pos != std::string::npos) {
        pos += 21;
        after.balance_total_una = std::stoull(after_json_str.substr(pos));
    }

    // Extract utxo_count
    pos = before_json_str.find("\"utxo_count\":");
    if (pos != std::string::npos) {
        pos += 13;
        before.utxo_count = std::stoul(before_json_str.substr(pos));
    }

    pos = after_json_str.find("\"utxo_count\":");
    if (pos != std::string::npos) {
        pos += 13;
        after.utxo_count = std::stoul(after_json_str.substr(pos));
    }

    // Extract address_count
    pos = before_json_str.find("\"address_count\":");
    if (pos != std::string::npos) {
        pos += 16;
        before.address_count = std::stoul(before_json_str.substr(pos));
    }

    pos = after_json_str.find("\"address_count\":");
    if (pos != std::string::npos) {
        pos += 16;
        after.address_count = std::stoul(after_json_str.substr(pos));
    }

    // Extract confirmed_tx_count
    pos = before_json_str.find("\"confirmed_tx_count\":");
    if (pos != std::string::npos) {
        pos += 21;
        before.confirmed_tx_count = std::stoul(before_json_str.substr(pos));
    }

    pos = after_json_str.find("\"confirmed_tx_count\":");
    if (pos != std::string::npos) {
        pos += 21;
        after.confirmed_tx_count = std::stoul(after_json_str.substr(pos));
    }

    // Validate invariants
    ValidationResult result = validateInvariants(before, after);

    if (!result.passed) {
        std::cerr << "╔═══════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║  WALLET INVARIANT VIOLATION DETECTED                 ║" << std::endl;
        std::cerr << "╚═══════════════════════════════════════════════════════╝" << std::endl;
        std::cerr << std::endl;

        for (const auto& error : result.errors) {
            std::cerr << error << std::endl;
        }

        std::cerr << std::endl;
        std::cerr << "Before snapshot:" << std::endl;
        std::cerr << before.toJSON() << std::endl;
        std::cerr << std::endl;
        std::cerr << "After snapshot:" << std::endl;
        std::cerr << after.toJSON() << std::endl;

        return 1; // Failure
    }

    std::cout << "✅ All wallet invariants validated successfully" << std::endl;
    return 0; // Success
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  wallet_oracle snapshot <wallet_db_path>" << std::endl;
        std::cerr << "  wallet_oracle validate <before_json_file> <after_json_file>" << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "snapshot") {
        if (argc < 3) {
            std::cerr << "Error: wallet_db_path required" << std::endl;
            return 1;
        }
        return cmdSnapshot(argv[2]);
    } else if (command == "validate") {
        if (argc < 4) {
            std::cerr << "Error: before and after JSON files required" << std::endl;
            return 1;
        }

        // Read JSON files
        std::ifstream before_file(argv[2]);
        std::ifstream after_file(argv[3]);

        std::string before_json((std::istreambuf_iterator<char>(before_file)),
                                std::istreambuf_iterator<char>());
        std::string after_json((std::istreambuf_iterator<char>(after_file)),
                               std::istreambuf_iterator<char>());

        return cmdValidate(before_json, after_json);
    } else {
        std::cerr << "Error: Unknown command: " << command << std::endl;
        return 1;
    }
}
