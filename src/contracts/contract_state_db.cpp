#include "contracts/contract_state_db.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <sqlite3.h>
#include <sstream>
#include <iomanip>
#include <ctime>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

ContractStateDB::ContractStateDB()
    : m_db(nullptr)
    , m_stmt_create_contract(nullptr)
    , m_stmt_get_contract(nullptr)
    , m_stmt_update_contract(nullptr)
    , m_stmt_add_history(nullptr)
    , m_stmt_get_history(nullptr)
    , m_stmt_add_commitment(nullptr)
    , m_stmt_get_commitment(nullptr)
    , m_stmt_update_confirmations(nullptr) {
}

ContractStateDB::~ContractStateDB() {
    close();
}

bool ContractStateDB::open(const std::string& db_path) {
    if (m_db != nullptr) {
        g_logger.warning("[ContractStateDB] Database already open");
        return true;
    }
    
    int rc = sqlite3_open(db_path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to open database: " + std::string(sqlite3_errmsg(m_db)));
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    
    // Enable WAL mode for better concurrency
    char* err_msg = nullptr;
    rc = sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.warning("[ContractStateDB] Failed to enable WAL mode: " + std::string(err_msg));
        sqlite3_free(err_msg);
    }
    
    // Create schema
    if (!createSchema()) {
        g_logger.error("[ContractStateDB] Failed to create schema");
        close();
        return false;
    }
    
    // Prepare statements
    if (!prepareStatements()) {
        g_logger.error("[ContractStateDB] Failed to prepare statements");
        close();
        return false;
    }
    
    g_logger.info("[ContractStateDB] Database opened: " + db_path);
    return true;
}

void ContractStateDB::close() {
    finalizeStatements();
    
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
        g_logger.info("[ContractStateDB] Database closed");
    }
}

bool ContractStateDB::createSchema() {
    const char* sql = R"(
        -- Contracts table
        CREATE TABLE IF NOT EXISTS contracts (
            contract_id TEXT PRIMARY KEY,
            contract_type TEXT NOT NULL,
            state_hash TEXT NOT NULL,
            merkle_root TEXT,
            commitment_txid TEXT,
            status TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            contract_data TEXT NOT NULL,
            party_a_address TEXT NOT NULL,
            party_b_address TEXT,
            mediator_address TEXT,
            lock_txid TEXT,
            settlement_txid TEXT
        );
        
        CREATE INDEX IF NOT EXISTS idx_contracts_status ON contracts(status);
        CREATE INDEX IF NOT EXISTS idx_contracts_type ON contracts(contract_type);
        CREATE INDEX IF NOT EXISTS idx_contracts_commitment ON contracts(commitment_txid);
        CREATE INDEX IF NOT EXISTS idx_contracts_party_a ON contracts(party_a_address);
        CREATE INDEX IF NOT EXISTS idx_contracts_party_b ON contracts(party_b_address);
        
        -- State history table
        CREATE TABLE IF NOT EXISTS contract_state_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            contract_id TEXT NOT NULL,
            state_hash TEXT NOT NULL,
            commitment_txid TEXT NOT NULL,
            state_data TEXT NOT NULL,
            transition_type TEXT NOT NULL,
            transitioned_by TEXT,
            block_height INTEGER,
            timestamp INTEGER NOT NULL,
            FOREIGN KEY (contract_id) REFERENCES contracts(contract_id)
        );
        
        CREATE INDEX IF NOT EXISTS idx_state_history_contract ON contract_state_history(contract_id);
        CREATE INDEX IF NOT EXISTS idx_state_history_txid ON contract_state_history(commitment_txid);
        CREATE INDEX IF NOT EXISTS idx_state_history_timestamp ON contract_state_history(timestamp);
        
        -- On-chain commitments table
        CREATE TABLE IF NOT EXISTS onchain_commitments (
            commitment_txid TEXT PRIMARY KEY,
            contract_id TEXT NOT NULL,
            state_hash TEXT NOT NULL,
            merkle_root TEXT NOT NULL,
            block_height INTEGER,
            block_hash TEXT,
            confirmations INTEGER DEFAULT 0,
            commitment_data TEXT,
            created_at INTEGER NOT NULL,
            FOREIGN KEY (contract_id) REFERENCES contracts(contract_id)
        );
        
        CREATE INDEX IF NOT EXISTS idx_commitments_contract ON onchain_commitments(contract_id);
        CREATE INDEX IF NOT EXISTS idx_commitments_height ON onchain_commitments(block_height);
    )";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to create schema: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

bool ContractStateDB::prepareStatements() {
    // Create contract
    const char* sql_create = R"(
        INSERT INTO contracts (
            contract_id, contract_type, state_hash, merkle_root, commitment_txid,
            status, created_at, updated_at, contract_data,
            party_a_address, party_b_address, mediator_address,
            lock_txid, settlement_txid
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_create, -1, &m_stmt_create_contract, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare create_contract statement");
        return false;
    }
    
    // Get contract
    const char* sql_get = "SELECT * FROM contracts WHERE contract_id = ?";
    if (sqlite3_prepare_v2(m_db, sql_get, -1, &m_stmt_get_contract, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare get_contract statement");
        return false;
    }
    
    // Update contract
    const char* sql_update = R"(
        UPDATE contracts SET
            state_hash = ?, merkle_root = ?, commitment_txid = ?,
            status = ?, updated_at = ?, contract_data = ?,
            settlement_txid = ?
        WHERE contract_id = ?
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_update, -1, &m_stmt_update_contract, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare update_contract statement");
        return false;
    }
    
    // Add state history
    const char* sql_history = R"(
        INSERT INTO contract_state_history (
            contract_id, state_hash, commitment_txid, state_data,
            transition_type, transitioned_by, block_height, timestamp
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_history, -1, &m_stmt_add_history, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare add_history statement");
        return false;
    }
    
    // Get state history
    const char* sql_get_history = R"(
        SELECT * FROM contract_state_history
        WHERE contract_id = ?
        ORDER BY timestamp ASC
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_get_history, -1, &m_stmt_get_history, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare get_history statement");
        return false;
    }
    
    // Add commitment
    const char* sql_commitment = R"(
        INSERT INTO onchain_commitments (
            commitment_txid, contract_id, state_hash, merkle_root,
            block_height, block_hash, confirmations, commitment_data, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_commitment, -1, &m_stmt_add_commitment, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare add_commitment statement");
        return false;
    }
    
    // Get commitment
    const char* sql_get_commitment = "SELECT * FROM onchain_commitments WHERE commitment_txid = ?";
    if (sqlite3_prepare_v2(m_db, sql_get_commitment, -1, &m_stmt_get_commitment, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare get_commitment statement");
        return false;
    }
    
    // Update confirmations
    const char* sql_update_conf = R"(
        UPDATE onchain_commitments
        SET confirmations = ?, block_height = ?, block_hash = ?
        WHERE commitment_txid = ?
    )";
    
    if (sqlite3_prepare_v2(m_db, sql_update_conf, -1, &m_stmt_update_confirmations, nullptr) != SQLITE_OK) {
        g_logger.error("[ContractStateDB] Failed to prepare update_confirmations statement");
        return false;
    }
    
    return true;
}

void ContractStateDB::finalizeStatements() {
    if (m_stmt_create_contract) sqlite3_finalize(m_stmt_create_contract);
    if (m_stmt_get_contract) sqlite3_finalize(m_stmt_get_contract);
    if (m_stmt_update_contract) sqlite3_finalize(m_stmt_update_contract);
    if (m_stmt_add_history) sqlite3_finalize(m_stmt_add_history);
    if (m_stmt_get_history) sqlite3_finalize(m_stmt_get_history);
    if (m_stmt_add_commitment) sqlite3_finalize(m_stmt_add_commitment);
    if (m_stmt_get_commitment) sqlite3_finalize(m_stmt_get_commitment);
    if (m_stmt_update_confirmations) sqlite3_finalize(m_stmt_update_confirmations);
    
    m_stmt_create_contract = nullptr;
    m_stmt_get_contract = nullptr;
    m_stmt_update_contract = nullptr;
    m_stmt_add_history = nullptr;
    m_stmt_get_history = nullptr;
    m_stmt_add_commitment = nullptr;
    m_stmt_get_commitment = nullptr;
    m_stmt_update_confirmations = nullptr;
}

// Helper functions
std::string ContractStateDB::contractTypeToString(ContractType type) const {
    switch (type) {
        case ContractType::ESCROW: return "escrow";
        case ContractType::LENDING: return "lending";
        case ContractType::DAO_GOVERNANCE: return "dao";
        default: return "unknown";
    }
}

ContractType ContractStateDB::stringToContractType(const std::string& str) const {
    if (str == "escrow") return ContractType::ESCROW;
    if (str == "lending") return ContractType::LENDING;
    if (str == "dao") return ContractType::DAO_GOVERNANCE;
    return ContractType::ESCROW; // default
}

std::string ContractStateDB::contractStatusToString(ContractStatus status) const {
    switch (status) {
        case ContractStatus::PENDING: return "pending";
        case ContractStatus::ACTIVE: return "active";
        case ContractStatus::DISPUTED: return "disputed";
        case ContractStatus::SETTLED: return "settled";
        case ContractStatus::CANCELLED: return "cancelled";
        default: return "unknown";
    }
}

ContractStatus ContractStateDB::stringToContractStatus(const std::string& str) const {
    if (str == "pending") return ContractStatus::PENDING;
    if (str == "active") return ContractStatus::ACTIVE;
    if (str == "disputed") return ContractStatus::DISPUTED;
    if (str == "settled") return ContractStatus::SETTLED;
    if (str == "cancelled") return ContractStatus::CANCELLED;
    return ContractStatus::PENDING; // default
}

std::string ContractStateDB::transitionTypeToString(TransitionType type) const {
    switch (type) {
        case TransitionType::CREATE: return "create";
        case TransitionType::UPDATE: return "update";
        case TransitionType::DISPUTE: return "dispute";
        case TransitionType::SETTLE: return "settle";
        case TransitionType::CANCEL: return "cancel";
        default: return "unknown";
    }
}

TransitionType ContractStateDB::stringToTransitionType(const std::string& str) const {
    if (str == "create") return TransitionType::CREATE;
    if (str == "update") return TransitionType::UPDATE;
    if (str == "dispute") return TransitionType::DISPUTE;
    if (str == "settle") return TransitionType::SETTLE;
    if (str == "cancel") return TransitionType::CANCEL;
    return TransitionType::CREATE; // default
}

// Contract management
bool ContractStateDB::createContract(const ContractState& contract) {
    if (!m_stmt_create_contract) return false;
    
    sqlite3_reset(m_stmt_create_contract);
    
    auto created_ts = std::chrono::duration_cast<std::chrono::seconds>(
        contract.created_at.time_since_epoch()).count();
    auto updated_ts = std::chrono::duration_cast<std::chrono::seconds>(
        contract.updated_at.time_since_epoch()).count();
    
    sqlite3_bind_text(m_stmt_create_contract, 1, contract.contract_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 2, contractTypeToString(contract.contract_type).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 3, contract.state_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 4, contract.merkle_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 5, contract.commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 6, contractStatusToString(contract.status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(m_stmt_create_contract, 7, created_ts);
    sqlite3_bind_int64(m_stmt_create_contract, 8, updated_ts);
    sqlite3_bind_text(m_stmt_create_contract, 9, contract.contract_data.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 10, contract.party_a_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 11, contract.party_b_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 12, contract.mediator_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 13, contract.lock_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_create_contract, 14, contract.settlement_txid.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(m_stmt_create_contract);
    if (rc != SQLITE_DONE) {
        g_logger.error("[ContractStateDB] Failed to create contract: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

bool ContractStateDB::getContract(const std::string& contract_id, ContractState& out) const {
    if (!m_stmt_get_contract) return false;
    
    sqlite3_reset(m_stmt_get_contract);
    sqlite3_bind_text(m_stmt_get_contract, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(m_stmt_get_contract);
    if (rc != SQLITE_ROW) {
        return false;
    }
    
    // Parse result
    out.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 0));
    out.contract_type = stringToContractType(reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 1)));
    out.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 2));
    out.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 3));
    out.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 4));
    out.status = stringToContractStatus(reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 5)));
    
    int64_t created_ts = sqlite3_column_int64(m_stmt_get_contract, 6);
    int64_t updated_ts = sqlite3_column_int64(m_stmt_get_contract, 7);
    out.created_at = std::chrono::system_clock::from_time_t(created_ts);
    out.updated_at = std::chrono::system_clock::from_time_t(updated_ts);
    
    out.contract_data = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 8));
    out.party_a_address = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 9));
    out.party_b_address = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 10));
    out.mediator_address = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 11));
    out.lock_txid = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 12));
    out.settlement_txid = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_contract, 13));
    
    return true;
}

bool ContractStateDB::updateContract(const std::string& contract_id, const ContractState& contract) {
    if (!m_stmt_update_contract) return false;
    
    sqlite3_reset(m_stmt_update_contract);
    
    auto updated_ts = std::chrono::duration_cast<std::chrono::seconds>(
        contract.updated_at.time_since_epoch()).count();
    
    sqlite3_bind_text(m_stmt_update_contract, 1, contract.state_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_contract, 2, contract.merkle_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_contract, 3, contract.commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_contract, 4, contractStatusToString(contract.status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(m_stmt_update_contract, 5, updated_ts);
    sqlite3_bind_text(m_stmt_update_contract, 6, contract.contract_data.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_contract, 7, contract.settlement_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_contract, 8, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(m_stmt_update_contract);
    if (rc != SQLITE_DONE) {
        g_logger.error("[ContractStateDB] Failed to update contract: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

bool ContractStateDB::deleteContract(const std::string& contract_id) {
    const char* sql = "DELETE FROM contracts WHERE contract_id = ?";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

// State history
bool ContractStateDB::addStateHistory(const StateHistoryEntry& entry) {
    if (!m_stmt_add_history) return false;
    
    sqlite3_reset(m_stmt_add_history);
    
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        entry.timestamp.time_since_epoch()).count();
    
    sqlite3_bind_text(m_stmt_add_history, 1, entry.contract_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_history, 2, entry.state_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_history, 3, entry.commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_history, 4, entry.state_data.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_history, 5, transitionTypeToString(entry.transition_type).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_history, 6, entry.transitioned_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_add_history, 7, entry.block_height);
    sqlite3_bind_int64(m_stmt_add_history, 8, timestamp);
    
    int rc = sqlite3_step(m_stmt_add_history);
    if (rc != SQLITE_DONE) {
        g_logger.error("[ContractStateDB] Failed to add state history: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

std::vector<StateHistoryEntry> ContractStateDB::getStateHistory(const std::string& contract_id) const {
    std::vector<StateHistoryEntry> result;
    
    if (!m_stmt_get_history) return result;
    
    sqlite3_reset(m_stmt_get_history);
    sqlite3_bind_text(m_stmt_get_history, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(m_stmt_get_history) == SQLITE_ROW) {
        StateHistoryEntry entry;
        entry.id = sqlite3_column_int64(m_stmt_get_history, 0);
        entry.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 1));
        entry.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 2));
        entry.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 3));
        entry.state_data = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 4));
        entry.transition_type = stringToTransitionType(reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 5)));
        entry.transitioned_by = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_history, 6));
        entry.block_height = sqlite3_column_int(m_stmt_get_history, 7);
        
        int64_t timestamp = sqlite3_column_int64(m_stmt_get_history, 8);
        entry.timestamp = std::chrono::system_clock::from_time_t(timestamp);
        
        result.push_back(entry);
    }
    
    return result;
}

StateHistoryEntry ContractStateDB::getLatestState(const std::string& contract_id) const {
    StateHistoryEntry entry;
    
    const char* sql = R"(
        SELECT * FROM contract_state_history
        WHERE contract_id = ?
        ORDER BY timestamp DESC
        LIMIT 1
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return entry;
    }
    
    sqlite3_bind_text(stmt, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        entry.id = sqlite3_column_int64(stmt, 0);
        entry.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        entry.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        entry.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        entry.state_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        entry.transition_type = stringToTransitionType(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        entry.transitioned_by = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        entry.block_height = sqlite3_column_int(stmt, 7);
        
        int64_t timestamp = sqlite3_column_int64(stmt, 8);
        entry.timestamp = std::chrono::system_clock::from_time_t(timestamp);
    }
    
    sqlite3_finalize(stmt);
    return entry;
}

// On-chain commitments
bool ContractStateDB::addCommitment(const OnChainCommitment& commitment) {
    if (!m_stmt_add_commitment) return false;
    
    sqlite3_reset(m_stmt_add_commitment);
    
    auto created_ts = std::chrono::duration_cast<std::chrono::seconds>(
        commitment.created_at.time_since_epoch()).count();
    
    sqlite3_bind_text(m_stmt_add_commitment, 1, commitment.commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_commitment, 2, commitment.contract_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_commitment, 3, commitment.state_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_add_commitment, 4, commitment.merkle_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_add_commitment, 5, commitment.block_height);
    sqlite3_bind_text(m_stmt_add_commitment, 6, commitment.block_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_stmt_add_commitment, 7, commitment.confirmations);
    sqlite3_bind_text(m_stmt_add_commitment, 8, commitment.commitment_data.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(m_stmt_add_commitment, 9, created_ts);
    
    int rc = sqlite3_step(m_stmt_add_commitment);
    if (rc != SQLITE_DONE) {
        g_logger.error("[ContractStateDB] Failed to add commitment: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

bool ContractStateDB::getCommitment(const std::string& commitment_txid, OnChainCommitment& out) const {
    if (!m_stmt_get_commitment) return false;
    
    sqlite3_reset(m_stmt_get_commitment);
    sqlite3_bind_text(m_stmt_get_commitment, 1, commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(m_stmt_get_commitment);
    if (rc != SQLITE_ROW) {
        return false;
    }
    
    out.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 0));
    out.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 1));
    out.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 2));
    out.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 3));
    out.block_height = sqlite3_column_int(m_stmt_get_commitment, 4);
    out.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 5));
    out.confirmations = sqlite3_column_int(m_stmt_get_commitment, 6);
    out.commitment_data = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_commitment, 7));
    
    int64_t created_ts = sqlite3_column_int64(m_stmt_get_commitment, 8);
    out.created_at = std::chrono::system_clock::from_time_t(created_ts);
    
    return true;
}

std::vector<OnChainCommitment> ContractStateDB::getContractCommitments(const std::string& contract_id) const {
    std::vector<OnChainCommitment> result;
    
    const char* sql = R"(
        SELECT * FROM onchain_commitments
        WHERE contract_id = ?
        ORDER BY block_height ASC
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    
    sqlite3_bind_text(stmt, 1, contract_id.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnChainCommitment commitment;
        commitment.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        commitment.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        commitment.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        commitment.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        commitment.block_height = sqlite3_column_int(stmt, 4);
        commitment.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        commitment.confirmations = sqlite3_column_int(stmt, 6);
        commitment.commitment_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        
        int64_t created_ts = sqlite3_column_int64(stmt, 8);
        commitment.created_at = std::chrono::system_clock::from_time_t(created_ts);
        
        result.push_back(commitment);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool ContractStateDB::updateCommitmentConfirmations(const std::string& commitment_txid, uint32_t confirmations) {
    if (!m_stmt_update_confirmations) return false;
    
    // Get current commitment to update block info
    OnChainCommitment commitment;
    if (!getCommitment(commitment_txid, commitment)) {
        return false;
    }
    
    sqlite3_reset(m_stmt_update_confirmations);
    sqlite3_bind_int(m_stmt_update_confirmations, 1, confirmations);
    sqlite3_bind_int(m_stmt_update_confirmations, 2, commitment.block_height);
    sqlite3_bind_text(m_stmt_update_confirmations, 3, commitment.block_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_stmt_update_confirmations, 4, commitment_txid.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(m_stmt_update_confirmations);
    if (rc != SQLITE_DONE) {
        g_logger.error("[ContractStateDB] Failed to update confirmations: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

// Query operations
std::vector<ContractState> ContractStateDB::getContractsByType(ContractType type) const {
    std::vector<ContractState> result;
    
    const char* sql = "SELECT * FROM contracts WHERE contract_type = ?";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    
    sqlite3_bind_text(stmt, 1, contractTypeToString(type).c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ContractState contract;
        contract.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        contract.contract_type = stringToContractType(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        contract.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        contract.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        contract.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        contract.status = stringToContractStatus(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        
        int64_t created_ts = sqlite3_column_int64(stmt, 6);
        int64_t updated_ts = sqlite3_column_int64(stmt, 7);
        contract.created_at = std::chrono::system_clock::from_time_t(created_ts);
        contract.updated_at = std::chrono::system_clock::from_time_t(updated_ts);
        
        contract.contract_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        contract.party_a_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        contract.party_b_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        contract.mediator_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        contract.lock_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        contract.settlement_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        
        result.push_back(contract);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ContractState> ContractStateDB::getContractsByStatus(ContractStatus status) const {
    std::vector<ContractState> result;
    
    const char* sql = "SELECT * FROM contracts WHERE status = ?";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    
    sqlite3_bind_text(stmt, 1, contractStatusToString(status).c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ContractState contract;
        // Same parsing as getContractsByType
        contract.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        contract.contract_type = stringToContractType(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        contract.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        contract.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        contract.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        contract.status = stringToContractStatus(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        
        int64_t created_ts = sqlite3_column_int64(stmt, 6);
        int64_t updated_ts = sqlite3_column_int64(stmt, 7);
        contract.created_at = std::chrono::system_clock::from_time_t(created_ts);
        contract.updated_at = std::chrono::system_clock::from_time_t(updated_ts);
        
        contract.contract_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        contract.party_a_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        contract.party_b_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        contract.mediator_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        contract.lock_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        contract.settlement_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        
        result.push_back(contract);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ContractState> ContractStateDB::getContractsByParty(const std::string& address) const {
    std::vector<ContractState> result;
    
    const char* sql = R"(
        SELECT * FROM contracts
        WHERE party_a_address = ? OR party_b_address = ? OR mediator_address = ?
    )";
    sqlite3_stmt* stmt = nullptr;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    
    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, address.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ContractState contract;
        // Same parsing as above
        contract.contract_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        contract.contract_type = stringToContractType(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        contract.state_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        contract.merkle_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        contract.commitment_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        contract.status = stringToContractStatus(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        
        int64_t created_ts = sqlite3_column_int64(stmt, 6);
        int64_t updated_ts = sqlite3_column_int64(stmt, 7);
        contract.created_at = std::chrono::system_clock::from_time_t(created_ts);
        contract.updated_at = std::chrono::system_clock::from_time_t(updated_ts);
        
        contract.contract_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        contract.party_a_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        contract.party_b_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        contract.mediator_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        contract.lock_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        contract.settlement_txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        
        result.push_back(contract);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

// State verification
bool ContractStateDB::verifyState(const std::string& contract_id) const {
    // Get contract
    ContractState contract;
    if (!getContract(contract_id, contract)) {
        return false;
    }
    
    // Calculate current state hash
    std::string calculated_hash = calculateStateHash(contract_id);
    
    // Compare with stored hash
    return calculated_hash == contract.state_hash;
}

std::string ContractStateDB::calculateStateHash(const std::string& contract_id) const {
    // Get contract
    ContractState contract;
    if (!getContract(contract_id, contract)) {
        return "";
    }
    
    // Build state JSON string for hashing
    std::ostringstream oss;
    oss << contract.contract_id
        << contractTypeToString(contract.contract_type)  // Convert enum to string
        << contract.contract_data
        << contractStatusToString(contract.status)  // Convert enum to string
        << contract.party_a_address
        << contract.party_b_address
        << contract.mediator_address;
    
    std::string state_string = oss.str();
    
    // Calculate SHA256 hash
    uint8_t hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(state_string.c_str()), state_string.length()).Finalize(hash);
    
    // Convert to hex string
    std::ostringstream hex_oss;
    for (int i = 0; i < 32; i++) {
        hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return hex_oss.str();
}

// Statistics
ContractStateDB::Stats ContractStateDB::getStats() const {
    Stats stats = {0, 0, 0, 0};
    
    const char* sql = R"(
        SELECT 
            (SELECT COUNT(*) FROM contracts) as total_contracts,
            (SELECT COUNT(*) FROM contracts WHERE status = 'active') as active_contracts,
            (SELECT COUNT(*) FROM onchain_commitments) as total_commitments,
            (SELECT COUNT(*) FROM contract_state_history) as total_transitions
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.total_contracts = sqlite3_column_int64(stmt, 0);
        stats.active_contracts = sqlite3_column_int64(stmt, 1);
        stats.total_commitments = sqlite3_column_int64(stmt, 2);
        stats.total_state_transitions = sqlite3_column_int64(stmt, 3);
    }
    
    sqlite3_finalize(stmt);
    return stats;
}

} // namespace contracts
} // namespace dinero

