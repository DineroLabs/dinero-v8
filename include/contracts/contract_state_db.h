#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>

// Forward declaration for sqlite3
struct sqlite3;
struct sqlite3_stmt;

namespace dinero {
namespace contracts {

// Contract types
enum class ContractType {
    ESCROW = 1,
    LENDING = 2,
    DAO_GOVERNANCE = 3
};

// Contract status
enum class ContractStatus {
    PENDING = 1,
    ACTIVE = 2,
    DISPUTED = 3,
    SETTLED = 4,
    CANCELLED = 5
};

// State transition types
enum class TransitionType {
    CREATE = 1,
    UPDATE = 2,
    DISPUTE = 3,
    SETTLE = 4,
    CANCEL = 5
};

// Contract state structure
struct ContractState {
    std::string contract_id;
    ContractType contract_type;
    std::string state_hash;           // SHA256 of current state JSON
    std::string merkle_root;          // Merkle root of state tree
    std::string commitment_txid;      // Latest commitment transaction ID
    ContractStatus status;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    // Contract-specific data (JSON string)
    std::string contract_data;
    
    // Parties
    std::string party_a_address;
    std::string party_b_address;
    std::string mediator_address;
    
    // On-chain references
    std::string lock_txid;            // P2SH lock transaction
    std::string settlement_txid;      // Final settlement transaction
    
    ContractState() : contract_type(ContractType::ESCROW), 
                      status(ContractStatus::PENDING) {
        auto now = std::chrono::system_clock::now();
        created_at = now;
        updated_at = now;
    }
};

// State history entry
struct StateHistoryEntry {
    int64_t id;
    std::string contract_id;
    std::string state_hash;
    std::string commitment_txid;
    std::string state_data;          // JSON state snapshot
    TransitionType transition_type;
    std::string transitioned_by;     // Address that triggered transition
    uint32_t block_height;
    std::chrono::system_clock::time_point timestamp;
    
    StateHistoryEntry() : id(0), block_height(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// On-chain commitment entry
struct OnChainCommitment {
    std::string commitment_txid;
    std::string contract_id;
    std::string state_hash;
    std::string merkle_root;
    uint32_t block_height;
    std::string block_hash;
    uint32_t confirmations;
    std::string commitment_data;      // OP_RETURN data (hex)
    std::chrono::system_clock::time_point created_at;
    
    OnChainCommitment() : block_height(0), confirmations(0) {
        created_at = std::chrono::system_clock::now();
    }
};

// Contract State Database
// Manages auxiliary state database for marketplace contracts
class ContractStateDB {
public:
    ContractStateDB();
    ~ContractStateDB();
    
    // Database lifecycle
    bool open(const std::string& db_path);
    void close();
    bool isOpen() const { return m_db != nullptr; }
    
    // Contract management
    bool createContract(const ContractState& contract);
    bool getContract(const std::string& contract_id, ContractState& out) const;
    bool updateContract(const std::string& contract_id, const ContractState& contract);
    bool deleteContract(const std::string& contract_id);
    
    // State history
    bool addStateHistory(const StateHistoryEntry& entry);
    std::vector<StateHistoryEntry> getStateHistory(const std::string& contract_id) const;
    StateHistoryEntry getLatestState(const std::string& contract_id) const;
    
    // On-chain commitments
    bool addCommitment(const OnChainCommitment& commitment);
    bool getCommitment(const std::string& commitment_txid, OnChainCommitment& out) const;
    std::vector<OnChainCommitment> getContractCommitments(const std::string& contract_id) const;
    bool updateCommitmentConfirmations(const std::string& commitment_txid, uint32_t confirmations);
    
    // Query operations
    std::vector<ContractState> getContractsByType(ContractType type) const;
    std::vector<ContractState> getContractsByStatus(ContractStatus status) const;
    std::vector<ContractState> getContractsByParty(const std::string& address) const;
    
    // State verification
    bool verifyState(const std::string& contract_id) const;
    std::string calculateStateHash(const std::string& contract_id) const;
    
    // Statistics
    struct Stats {
        uint64_t total_contracts;
        uint64_t active_contracts;
        uint64_t total_commitments;
        uint64_t total_state_transitions;
    };
    Stats getStats() const;
    
private:
    // Database operations
    bool createSchema();
    bool prepareStatements();
    void finalizeStatements();
    
    // Helper functions
    std::string contractTypeToString(ContractType type) const;
    ContractType stringToContractType(const std::string& str) const;
    std::string contractStatusToString(ContractStatus status) const;
    ContractStatus stringToContractStatus(const std::string& str) const;
    std::string transitionTypeToString(TransitionType type) const;
    TransitionType stringToTransitionType(const std::string& str) const;
    
    sqlite3* m_db;
    
    // Prepared statements
    sqlite3_stmt* m_stmt_create_contract;
    sqlite3_stmt* m_stmt_get_contract;
    sqlite3_stmt* m_stmt_update_contract;
    sqlite3_stmt* m_stmt_add_history;
    sqlite3_stmt* m_stmt_get_history;
    sqlite3_stmt* m_stmt_add_commitment;
    sqlite3_stmt* m_stmt_get_commitment;
    sqlite3_stmt* m_stmt_update_confirmations;
};

}} // namespace dinero::contracts

