#include "database.h"
#include <stdexcept>
#include <sstream>

namespace dinero {
namespace wallet {
namespace reference {

Database::Database(const std::string& db_path)
    : db_(nullptr)
    , db_path_(db_path)
    , in_transaction_(false) {

    int result = sqlite3_open(db_path.c_str(), &db_);
    if (result != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("Failed to open database: " + error);
    }

    // Enable WAL mode for better concurrency
    Execute("PRAGMA journal_mode=WAL");
    Execute("PRAGMA synchronous=NORMAL");
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::InitializeSchema() {
    BeginTransaction();

    try {
        // Create wallet_metadata table
        Execute(R"(
            CREATE TABLE IF NOT EXISTS wallet_metadata (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
        )");

        // Create utxos table
        Execute(R"(
            CREATE TABLE IF NOT EXISTS utxos (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL DEFAULT 1,
                txid TEXT NOT NULL,
                vout INTEGER NOT NULL,
                address TEXT NOT NULL DEFAULT '',
                amount INTEGER NOT NULL,
                script_pubkey TEXT NOT NULL,
                height INTEGER NOT NULL,
                is_coinbase INTEGER NOT NULL DEFAULT 0,
                is_mature INTEGER NOT NULL DEFAULT 0,
                is_spent INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL DEFAULT 0,
                UNIQUE(wallet_id, txid, vout)
            )
        )");

        // Create index for height-based queries
        Execute("CREATE INDEX IF NOT EXISTS idx_utxos_height ON utxos(height)");

        // Create spent_utxos table
        Execute(R"(
            CREATE TABLE IF NOT EXISTS spent_utxos (
                txid TEXT NOT NULL,
                vout INTEGER NOT NULL,
                spent_in_txid TEXT NOT NULL,
                spent_at_height INTEGER NOT NULL,
                PRIMARY KEY (txid, vout)
            )
        )");

        // Create transactions table
        Execute(R"(
            CREATE TABLE IF NOT EXISTS transactions (
                txid TEXT PRIMARY KEY,
                hex TEXT NOT NULL,
                amount_sent INTEGER NOT NULL,
                fee INTEGER NOT NULL,
                to_address TEXT NOT NULL,
                height INTEGER NOT NULL,
                timestamp INTEGER NOT NULL
            )
        )");

        // Create index for transaction listing (newest first)
        Execute("CREATE INDEX IF NOT EXISTS idx_transactions_timestamp ON transactions(timestamp DESC)");

        CommitTransaction();
    } catch (...) {
        RollbackTransaction();
        throw;
    }
}

void Database::BeginTransaction() {
    if (in_transaction_) {
        throw std::runtime_error("Transaction already in progress");
    }
    Execute("BEGIN TRANSACTION");
    in_transaction_ = true;
}

void Database::CommitTransaction() {
    if (!in_transaction_) {
        throw std::runtime_error("No transaction in progress");
    }
    Execute("COMMIT");
    in_transaction_ = false;
}

void Database::RollbackTransaction() {
    if (!in_transaction_) return;
    Execute("ROLLBACK");
    in_transaction_ = false;
}

bool Database::Execute(const std::string& sql) {
    char* error_msg = nullptr;
    int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error_msg);

    if (result != SQLITE_OK) {
        std::string error = error_msg ? error_msg : "Unknown error";
        sqlite3_free(error_msg);
        throw std::runtime_error("SQL execution failed: " + error);
    }

    return true;
}

void Database::CheckError(int result, const std::string& operation) {
    if (result != SQLITE_OK && result != SQLITE_ROW && result != SQLITE_DONE) {
        std::string error = sqlite3_errmsg(db_);
        throw std::runtime_error(operation + " failed: " + error);
    }
}

// Statement implementation
Database::Statement::Statement(sqlite3* db, const std::string& sql)
    : stmt_(nullptr) {
    int result = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
    if (result != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement: " + std::string(sqlite3_errmsg(db)));
    }
}

Database::Statement::~Statement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

void Database::Statement::BindText(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void Database::Statement::BindInt(int index, int value) {
    sqlite3_bind_int(stmt_, index, value);
}

void Database::Statement::BindInt64(int index, int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
}

void Database::Statement::BindBlob(int index, const std::vector<uint8_t>& value) {
    sqlite3_bind_blob(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT);
}

bool Database::Statement::Step() {
    int result = sqlite3_step(stmt_);
    if (result == SQLITE_ROW) {
        return true;
    } else if (result == SQLITE_DONE) {
        return false;
    } else {
        throw std::runtime_error("Step failed");
    }
}

void Database::Statement::Reset() {
    sqlite3_reset(stmt_);
}

std::string Database::Statement::GetText(int column) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
    return text ? text : "";
}

int Database::Statement::GetInt(int column) {
    return sqlite3_column_int(stmt_, column);
}

int64_t Database::Statement::GetInt64(int column) {
    return sqlite3_column_int64(stmt_, column);
}

std::vector<uint8_t> Database::Statement::GetBlob(int column) {
    const uint8_t* blob = reinterpret_cast<const uint8_t*>(sqlite3_column_blob(stmt_, column));
    int size = sqlite3_column_bytes(stmt_, column);
    return std::vector<uint8_t>(blob, blob + size);
}

std::unique_ptr<Database::Statement> Database::Prepare(const std::string& sql) {
    return std::make_unique<Statement>(db_, sql);
}

// Metadata operations
void Database::SetMetadata(const std::string& key, const std::string& value) {
    auto stmt = Prepare("INSERT OR REPLACE INTO wallet_metadata (key, value) VALUES (?, ?)");
    stmt->BindText(1, key);
    stmt->BindText(2, value);
    stmt->Step();
}

std::string Database::GetMetadata(const std::string& key, const std::string& default_value) {
    auto stmt = Prepare("SELECT value FROM wallet_metadata WHERE key = ?");
    stmt->BindText(1, key);
    if (stmt->Step()) {
        return stmt->GetText(0);
    }
    return default_value;
}

// UTXO operations
void Database::InsertUTXO(const UTXORow& utxo) {
    auto stmt = Prepare(R"(
        INSERT OR REPLACE INTO utxos (txid, vout, amount, script_pubkey, height, is_coinbase)
        VALUES (?, ?, ?, ?, ?, ?)
    )");
    stmt->BindText(1, utxo.txid);
    stmt->BindInt(2, utxo.vout);
    stmt->BindInt64(3, utxo.amount);
    stmt->BindText(4, utxo.script_pubkey);
    stmt->BindInt(5, utxo.height);
    stmt->BindInt(6, utxo.is_coinbase ? 1 : 0);
    stmt->Step();
}

void Database::DeleteUTXO(const std::string& txid, uint32_t vout) {
    auto stmt = Prepare("DELETE FROM utxos WHERE txid = ? AND vout = ?");
    stmt->BindText(1, txid);
    stmt->BindInt(2, vout);
    stmt->Step();
}

std::vector<Database::UTXORow> Database::GetAllUTXOs() {
    std::vector<UTXORow> utxos;
    auto stmt = Prepare("SELECT txid, vout, amount, script_pubkey, height, is_coinbase FROM utxos ORDER BY txid, vout");

    while (stmt->Step()) {
        UTXORow utxo;
        utxo.txid = stmt->GetText(0);
        utxo.vout = stmt->GetInt(1);
        utxo.amount = stmt->GetInt64(2);
        utxo.script_pubkey = stmt->GetText(3);
        utxo.height = stmt->GetInt(4);
        utxo.is_coinbase = stmt->GetInt(5) != 0;
        utxos.push_back(utxo);
    }

    return utxos;
}

std::vector<Database::UTXORow> Database::GetUTXOsByHeight(uint32_t min_height) {
    std::vector<UTXORow> utxos;
    auto stmt = Prepare("SELECT txid, vout, amount, script_pubkey, height, is_coinbase FROM utxos WHERE height >= ? ORDER BY txid, vout");
    stmt->BindInt(1, min_height);

    while (stmt->Step()) {
        UTXORow utxo;
        utxo.txid = stmt->GetText(0);
        utxo.vout = stmt->GetInt(1);
        utxo.amount = stmt->GetInt64(2);
        utxo.script_pubkey = stmt->GetText(3);
        utxo.height = stmt->GetInt(4);
        utxo.is_coinbase = stmt->GetInt(5) != 0;
        utxos.push_back(utxo);
    }

    return utxos;
}

// Spent UTXO operations
void Database::MarkUTXOSpent(const std::string& txid, uint32_t vout,
                            const std::string& spent_in_txid, uint32_t spent_at_height) {
    BeginTransaction();
    try {
        // Remove from utxos
        DeleteUTXO(txid, vout);

        // Add to spent_utxos
        auto stmt = Prepare(R"(
            INSERT OR REPLACE INTO spent_utxos (txid, vout, spent_in_txid, spent_at_height)
            VALUES (?, ?, ?, ?)
        )");
        stmt->BindText(1, txid);
        stmt->BindInt(2, vout);
        stmt->BindText(3, spent_in_txid);
        stmt->BindInt(4, spent_at_height);
        stmt->Step();

        CommitTransaction();
    } catch (...) {
        RollbackTransaction();
        throw;
    }
}

bool Database::IsUTXOSpent(const std::string& txid, uint32_t vout) {
    auto stmt = Prepare("SELECT 1 FROM spent_utxos WHERE txid = ? AND vout = ?");
    stmt->BindText(1, txid);
    stmt->BindInt(2, vout);
    return stmt->Step();
}

// Transaction operations
void Database::InsertTransaction(const TransactionRow& tx) {
    auto stmt = Prepare(R"(
        INSERT OR REPLACE INTO transactions (txid, hex, amount_sent, fee, to_address, height, timestamp)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )");
    stmt->BindText(1, tx.txid);
    stmt->BindText(2, tx.hex);
    stmt->BindInt64(3, tx.amount_sent);
    stmt->BindInt64(4, tx.fee);
    stmt->BindText(5, tx.to_address);
    stmt->BindInt(6, tx.height);
    stmt->BindInt64(7, tx.timestamp);
    stmt->Step();
}

Database::TransactionRow Database::GetTransaction(const std::string& txid) {
    auto stmt = Prepare("SELECT txid, hex, amount_sent, fee, to_address, height, timestamp FROM transactions WHERE txid = ?");
    stmt->BindText(1, txid);

    if (!stmt->Step()) {
        throw std::runtime_error("Transaction not found: " + txid);
    }

    TransactionRow tx;
    tx.txid = stmt->GetText(0);
    tx.hex = stmt->GetText(1);
    tx.amount_sent = stmt->GetInt64(2);
    tx.fee = stmt->GetInt64(3);
    tx.to_address = stmt->GetText(4);
    tx.height = stmt->GetInt(5);
    tx.timestamp = stmt->GetInt64(6);

    return tx;
}

std::vector<Database::TransactionRow> Database::GetTransactions(uint32_t limit, uint32_t offset) {
    std::vector<TransactionRow> transactions;

    std::string sql = "SELECT txid, hex, amount_sent, fee, to_address, height, timestamp FROM transactions ORDER BY timestamp DESC";
    if (limit > 0) {
        sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
    }

    auto stmt = Prepare(sql);

    while (stmt->Step()) {
        TransactionRow tx;
        tx.txid = stmt->GetText(0);
        tx.hex = stmt->GetText(1);
        tx.amount_sent = stmt->GetInt64(2);
        tx.fee = stmt->GetInt64(3);
        tx.to_address = stmt->GetText(4);
        tx.height = stmt->GetInt(5);
        tx.timestamp = stmt->GetInt64(6);
        transactions.push_back(tx);
    }

    return transactions;
}

bool Database::TransactionExists(const std::string& txid) {
    auto stmt = Prepare("SELECT 1 FROM transactions WHERE txid = ?");
    stmt->BindText(1, txid);
    return stmt->Step();
}

} // namespace reference
} // namespace wallet
} // namespace dinero
