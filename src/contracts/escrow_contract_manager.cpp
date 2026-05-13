#include "contracts/escrow_contract_manager.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

EscrowContractManager::EscrowContractManager(const std::string& db_path)
    : db_path_(db_path) {
}

EscrowContractManager::~EscrowContractManager() {
    if (db_) {
        db_->close();
    }
}

bool EscrowContractManager::initialize() {
    db_ = std::make_unique<ContractStateDB>();
    if (!db_->open(db_path_)) {
        g_logger.error("[EscrowContractManager] Failed to open database: " + db_path_);
        return false;
    }
    
    verifier_ = std::make_unique<StateVerifier>(*db_);
    
    g_logger.info("[EscrowContractManager] Initialized with database: " + db_path_);
    return true;
}

std::optional<std::string> EscrowContractManager::createEscrowContract(
    const std::string& seller_address,
    const std::string& buyer_address,
    const std::string& mediator_address,
    double amount,
    uint64_t duration_seconds,
    const std::string& offer_id) {
    
    if (!db_ || !db_->isOpen()) {
        g_logger.error("[EscrowContractManager] Database not initialized");
        return std::nullopt;
    }
    
    // Generate contract ID
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    std::ostringstream oss;
    oss << "escrow_" << std::hex << timestamp;
    std::string contract_id = oss.str();
    
    // Build contract data JSON
    std::string contract_data = buildEscrowContractData(
        seller_address, buyer_address, mediator_address,
        amount, duration_seconds, offer_id
    );
    
    // Calculate initial state hash
    std::string state_hash = calculateEscrowStateHash(contract_id, "pending", contract_data);
    
    // Create contract state
    ContractState contract;
    contract.contract_id = contract_id;
    contract.contract_type = ContractType::ESCROW;
    contract.state_hash = state_hash;
    contract.status = ContractStatus::PENDING;
    contract.contract_data = contract_data;
    contract.party_a_address = seller_address;
    contract.party_b_address = buyer_address;
    contract.mediator_address = mediator_address;
    
    auto now_time = std::chrono::system_clock::now();
    contract.created_at = now_time;
    contract.updated_at = now_time;
    
    // Create contract in database
    if (!db_->createContract(contract)) {
        g_logger.error("[EscrowContractManager] Failed to create contract");
        return std::nullopt;
    }
    
    // Record initial state history
    StateHistoryEntry history;
    history.contract_id = contract_id;
    history.state_hash = state_hash;
    history.commitment_txid = ""; // No commitment yet
    history.state_data = contract_data;
    history.transition_type = TransitionType::CREATE;
    history.transitioned_by = seller_address;
    history.block_height = 0;
    history.timestamp = now_time;
    
    if (!db_->addStateHistory(history)) {
        g_logger.warning("[EscrowContractManager] Failed to add initial state history");
    }
    
    g_logger.info("[EscrowContractManager] Created escrow contract: " + contract_id);
    
    return contract_id;
}

bool EscrowContractManager::updateEscrowState(
    const std::string& contract_id,
    const std::string& new_status,
    const std::string& transitioned_by,
    const std::string& transition_data) {
    
    if (!db_ || !db_->isOpen()) {
        g_logger.error("[EscrowContractManager] Database not initialized");
        return false;
    }
    
    // Get current contract
    ContractState contract;
    if (!db_->getContract(contract_id, contract)) {
        g_logger.error("[EscrowContractManager] Contract not found: " + contract_id);
        return false;
    }
    
    // Update status
    contract.status = escrowStatusToContractStatus(new_status);
    contract.updated_at = std::chrono::system_clock::now();
    
    // Update contract data if provided
    if (!transition_data.empty()) {
        contract.contract_data = transition_data;
    }
    
    // Recalculate state hash
    std::string new_state_hash = calculateEscrowStateHash(
        contract_id, new_status, contract.contract_data
    );
    contract.state_hash = new_state_hash;
    
    // Update contract in database
    if (!db_->updateContract(contract_id, contract)) {
        g_logger.error("[EscrowContractManager] Failed to update contract");
        return false;
    }
    
    // Record state history
    StateHistoryEntry history;
    history.contract_id = contract_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid; // Will be updated when commitment is recorded
    history.state_data = contract.contract_data;
    
    // Determine transition type
    if (new_status == "locked") {
        history.transition_type = TransitionType::UPDATE;
    } else if (new_status == "released") {
        history.transition_type = TransitionType::SETTLE;
    } else if (new_status == "refunded") {
        history.transition_type = TransitionType::CANCEL;
    } else if (new_status == "disputed") {
        history.transition_type = TransitionType::DISPUTE;
    } else {
        history.transition_type = TransitionType::UPDATE;
    }
    
    history.transitioned_by = transitioned_by;
    history.block_height = 0; // Will be updated when commitment is confirmed
    history.timestamp = std::chrono::system_clock::now();
    
    if (!db_->addStateHistory(history)) {
        g_logger.warning("[EscrowContractManager] Failed to add state history");
    }
    
    g_logger.info("[EscrowContractManager] Updated escrow state: " + contract_id + " -> " + new_status);
    
    return true;
}

std::optional<std::vector<uint8_t>> EscrowContractManager::createCommitment(
    const std::string& contract_id) {
    
    if (!db_ || !db_->isOpen()) {
        g_logger.error("[EscrowContractManager] Database not initialized");
        return std::nullopt;
    }
    
    // Get contract
    ContractState contract;
    if (!db_->getContract(contract_id, contract)) {
        g_logger.error("[EscrowContractManager] Contract not found: " + contract_id);
        return std::nullopt;
    }
    
    // Get state history for Merkle root calculation
    std::vector<StateHistoryEntry> history = db_->getStateHistory(contract_id);
    std::vector<std::string> state_hashes;
    for (const auto& entry : history) {
        state_hashes.push_back(entry.state_hash);
    }
    state_hashes.push_back(contract.state_hash); // Include current state
    
    // Calculate Merkle root
    std::string merkle_root = CommitmentTransactionBuilder::calculateMerkleRoot(state_hashes);
    
    // Build commitment data
    CommitmentTransactionBuilder::CommitmentData commitment;
    commitment.version = 0x01;
    
    // Contract ID hash (SHA256 of contract creation data)
    std::string contract_id_data = contract_id + contract.party_a_address + 
                                   std::to_string(contract.created_at.time_since_epoch().count());
    uint8_t contract_id_hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(contract_id_data.c_str()), 
                   contract_id_data.length()).Finalize(contract_id_hash);
    
    std::ostringstream contract_id_hex;
    for (int i = 0; i < 32; i++) {
        contract_id_hex << std::hex << std::setw(2) << std::setfill('0') 
                       << static_cast<int>(contract_id_hash[i]);
    }
    commitment.contract_id = contract_id_hex.str();
    
    commitment.state_hash = contract.state_hash;
    commitment.merkle_root = merkle_root;
    commitment.nonce = CommitmentTransactionBuilder::generateNonce();
    
    // Build commitment script
    std::vector<uint8_t> script = CommitmentTransactionBuilder::buildCommitmentScript(commitment);
    
    if (script.empty()) {
        g_logger.error("[EscrowContractManager] Failed to build commitment script");
        return std::nullopt;
    }
    
    // Update contract with Merkle root
    contract.merkle_root = merkle_root;
    db_->updateContract(contract_id, contract);
    
    g_logger.info("[EscrowContractManager] Created commitment for contract: " + contract_id);
    
    return script;
}

bool EscrowContractManager::recordCommitmentTransaction(
    const std::string& contract_id,
    const std::string& commitment_txid,
    uint32_t block_height,
    const std::string& block_hash) {
    
    if (!db_ || !db_->isOpen()) {
        return false;
    }
    
    // Get contract
    ContractState contract;
    if (!db_->getContract(contract_id, contract)) {
        return false;
    }
    
    // Create commitment entry
    OnChainCommitment commitment;
    commitment.commitment_txid = commitment_txid;
    commitment.contract_id = contract_id;
    commitment.state_hash = contract.state_hash;
    commitment.merkle_root = contract.merkle_root;
    commitment.block_height = block_height;
    commitment.block_hash = block_hash;
    commitment.confirmations = 0;
    commitment.created_at = std::chrono::system_clock::now();
    
    // Get commitment script for storage
    auto script_opt = createCommitment(contract_id);
    if (script_opt) {
        // Convert script to hex
        std::ostringstream hex_oss;
        for (uint8_t byte : *script_opt) {
            hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        commitment.commitment_data = hex_oss.str();
    }
    
    // Add commitment to database
    if (!db_->addCommitment(commitment)) {
        g_logger.error("[EscrowContractManager] Failed to record commitment");
        return false;
    }
    
    // Update contract with commitment TXID
    contract.commitment_txid = commitment_txid;
    if (!db_->updateContract(contract_id, contract)) {
        g_logger.warning("[EscrowContractManager] Failed to update contract commitment_txid");
    }
    
    // Update latest state history entry with commitment TXID
    StateHistoryEntry latest = db_->getLatestState(contract_id);
    if (latest.id > 0) {
        // Note: We'd need an update method for state history, but for now this is sufficient
        // The commitment_txid in contract is the source of truth
    }
    
    g_logger.info("[EscrowContractManager] Recorded commitment transaction: " + commitment_txid);
    
    return true;
}

bool EscrowContractManager::getEscrowContract(const std::string& contract_id, ContractState& out) const {
    if (!db_ || !db_->isOpen()) {
        return false;
    }
    
    return db_->getContract(contract_id, out);
}

std::vector<StateHistoryEntry> EscrowContractManager::getEscrowHistory(const std::string& contract_id) const {
    if (!db_ || !db_->isOpen()) {
        return {};
    }
    
    return db_->getStateHistory(contract_id);
}

bool EscrowContractManager::verifyEscrowState(const std::string& contract_id) const {
    if (!verifier_) {
        return false;
    }
    
    return verifier_->verifyContractState(contract_id);
}

StateVerifier::VerificationReport EscrowContractManager::getVerificationReport(const std::string& contract_id) const {
    if (!verifier_) {
        StateVerifier::VerificationReport report;
        report.errors.push_back("Verifier not initialized");
        return report;
    }
    
    return verifier_->generateReport(contract_id);
}

// Helper functions
ContractStatus EscrowContractManager::escrowStatusToContractStatus(const std::string& escrow_status) const {
    if (escrow_status == "pending") return ContractStatus::PENDING;
    if (escrow_status == "locked") return ContractStatus::ACTIVE;
    if (escrow_status == "released") return ContractStatus::SETTLED;
    if (escrow_status == "refunded") return ContractStatus::CANCELLED;
    if (escrow_status == "disputed") return ContractStatus::DISPUTED;
    return ContractStatus::PENDING;
}

std::string EscrowContractManager::buildEscrowContractData(
    const std::string& seller_address,
    const std::string& buyer_address,
    const std::string& mediator_address,
    double amount,
    uint64_t duration_seconds,
    const std::string& offer_id) const {
    
    // Build JSON-like contract data string
    // In production, use proper JSON library
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"escrow\","
        << "\"seller\":\"" << seller_address << "\","
        << "\"buyer\":\"" << buyer_address << "\","
        << "\"mediator\":\"" << mediator_address << "\","
        << "\"amount\":" << std::fixed << std::setprecision(8) << amount << ","
        << "\"duration_seconds\":" << duration_seconds << ","
        << "\"offer_id\":\"" << offer_id << "\""
        << "}";
    
    return oss.str();
}

std::string EscrowContractManager::calculateEscrowStateHash(
    const std::string& contract_id,
    const std::string& status,
    const std::string& contract_data) const {
    
    // Build state string for hashing
    std::ostringstream oss;
    oss << contract_id << status << contract_data;
    
    std::string state_string = oss.str();
    
    // Calculate SHA256 hash
    uint8_t hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(state_string.c_str()), 
                   state_string.length()).Finalize(hash);
    
    // Convert to hex string
    std::ostringstream hex_oss;
    for (int i = 0; i < 32; i++) {
        hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return hex_oss.str();
}

} // namespace contracts
} // namespace dinero

