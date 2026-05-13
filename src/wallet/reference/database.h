#ifndef DINEROCOIN_WALLET_REFERENCE_DATABASE_H
#define DINEROCOIN_WALLET_REFERENCE_DATABASE_H

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <sqlite3.h>

namespace dinero {
namespace wallet {
namespace reference {

/**
 * Database interface for reference wallet
 *
 * Schema (as defined in reference_wallet.md):
 *
 * CREATE TABLE wallet_metadata (
 *   key TEXT PRIMARY KEY,
 *   value TEXT NOT NULL
 * );
 *
 * CREATE TABLE utxos (
 *   txid TEXT NOT NULL,
 *   vout INTEGER NOT NULL,
 *   amount INTEGER NOT NULL,
 *   script_pubkey TEXT NOT NULL,
 *   height INTEGER NOT NULL,
 *   is_coinbase INTEGER NOT NULL,
 *   PRIMARY KEY (txid, vout)
 * );
 *
 * CREATE TABLE spent_utxos (
 *   txid TEXT NOT NULL,
 *   vout INTEGER NOT NULL,
 *   spent_in_txid TEXT NOT NULL,
 *   spent_at_height INTEGER NOT NULL,
 *   PRIMARY KEY (txid, vout)
 * );
 *
 * CREATE TABLE transactions (
 *   txid TEXT PRIMARY KEY,
 *   hex TEXT NOT NULL,
 *   amount_sent INTEGER NOT NULL,
 *   fee INTEGER NOT NULL,
 *   to_address TEXT NOT NULL,
 *   height INTEGER NOT NULL,
 *   timestamp INTEGER NOT NULL
 * );
 */
class Database {
public:
    /**
     * Open or create database
     * @param db_path Path to wallet.db file
     */
    explicit Database(const std::string& db_path);
    ~Database();

    // Disable copy/move
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /**
     * Initialize database schema
     * Creates tables if they don't exist
     */
    void InitializeSchema();

    /**
     * Begin transaction
     */
    void BeginTransaction();

    /**
     * Commit transaction
     */
    void CommitTransaction();

    /**
     * Rollback transaction
     */
    void RollbackTransaction();

    /**
     * Execute SQL query
     * @param sql SQL statement
     * @return true if successful
     */
    bool Execute(const std::string& sql);

    /**
     * Prepared statement wrapper for safe queries
     */
    class Statement {
    public:
        Statement(sqlite3* db, const std::string& sql);
        ~Statement();

        // Bind parameters (1-indexed)
        void BindText(int index, const std::string& value);
        void BindInt(int index, int value);
        void BindInt64(int index, int64_t value);
        void BindBlob(int index, const std::vector<uint8_t>& value);

        // Execute
        bool Step();  // Returns true if row available, false if done
        void Reset();

        // Get columns (0-indexed)
        std::string GetText(int column);
        int GetInt(int column);
        int64_t GetInt64(int column);
        std::vector<uint8_t> GetBlob(int column);

    private:
        sqlite3_stmt* stmt_;
    };

    /**
     * Create prepared statement
     * @param sql SQL with ? placeholders
     * @return Unique pointer to statement
     */
    std::unique_ptr<Statement> Prepare(const std::string& sql);

    // Metadata operations
    void SetMetadata(const std::string& key, const std::string& value);
    std::string GetMetadata(const std::string& key, const std::string& default_value = "");

    // UTXO operations
    struct UTXORow {
        std::string txid;
        uint32_t vout;
        uint64_t amount;
        std::string script_pubkey;
        uint32_t height;
        bool is_coinbase;
    };

    void InsertUTXO(const UTXORow& utxo);
    void DeleteUTXO(const std::string& txid, uint32_t vout);
    std::vector<UTXORow> GetAllUTXOs();
    std::vector<UTXORow> GetUTXOsByHeight(uint32_t min_height);

    // Spent UTXO operations
    void MarkUTXOSpent(
        const std::string& txid,
        uint32_t vout,
        const std::string& spent_in_txid,
        uint32_t spent_at_height
    );

    bool IsUTXOSpent(const std::string& txid, uint32_t vout);

    // Transaction operations
    struct TransactionRow {
        std::string txid;
        std::string hex;
        uint64_t amount_sent;
        uint64_t fee;
        std::string to_address;
        uint32_t height;
        int64_t timestamp;
    };

    void InsertTransaction(const TransactionRow& tx);
    TransactionRow GetTransaction(const std::string& txid);
    std::vector<TransactionRow> GetTransactions(uint32_t limit, uint32_t offset);
    bool TransactionExists(const std::string& txid);

private:
    sqlite3* db_;
    std::string db_path_;
    bool in_transaction_;

    // Helper to check errors
    void CheckError(int result, const std::string& operation);
};

} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_DATABASE_H
