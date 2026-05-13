#include "contracts/lending_contract_manager.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

LendingContractManager::LendingContractManager(ContractStateDB& db)
    : db_(db) {
    verifier_ = std::make_unique<StateVerifier>(db_);
}

std::optional<std::string> LendingContractManager::createLendingContract(
    const std::string& lender_address,
    const std::string& borrower_address,
    double principal_amount,
    double interest_rate,
    uint32_t term_months,
    LendingType lending_type,
    const std::string& collateral_address,
    double collateral_amount) {
    
    if (!db_.isOpen()) {
        g_logger.error("[LendingContractManager] Database not open");
        return std::nullopt;
    }
    
    // Validate parameters
    if (principal_amount <= 0 || interest_rate < 0 || term_months == 0) {
        g_logger.error("[LendingContractManager] Invalid loan parameters");
        return std::nullopt;
    }
    
    // Generate contract ID
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    std::ostringstream oss;
    oss << "lending_" << std::hex << timestamp;
    std::string contract_id = oss.str();
    
    // Build contract data JSON
    std::string contract_data = buildLendingContractData(
        lender_address, borrower_address, principal_amount,
        interest_rate, term_months, lending_type,
        collateral_address, collateral_amount
    );
    
    // Calculate initial state hash
    std::string state_hash = calculateLendingStateHash(contract_id, "pending", contract_data);
    
    // Create contract state
    ContractState contract;
    contract.contract_id = contract_id;
    contract.contract_type = ContractType::LENDING;
    contract.state_hash = state_hash;
    contract.status = ContractStatus::PENDING;
    contract.contract_data = contract_data;
    contract.party_a_address = lender_address;
    contract.party_b_address = borrower_address;
    contract.mediator_address = ""; // No mediator for lending
    
    auto now_time = std::chrono::system_clock::now();
    contract.created_at = now_time;
    contract.updated_at = now_time;
    
    // Create contract in database
    if (!db_.createContract(contract)) {
        g_logger.error("[LendingContractManager] Failed to create contract");
        return std::nullopt;
    }
    
    // Record initial state history
    StateHistoryEntry history;
    history.contract_id = contract_id;
    history.state_hash = state_hash;
    history.commitment_txid = "";
    history.state_data = contract_data;
    history.transition_type = TransitionType::CREATE;
    history.transitioned_by = lender_address;
    history.block_height = 0;
    history.timestamp = now_time;
    
    if (!db_.addStateHistory(history)) {
        g_logger.warning("[LendingContractManager] Failed to add initial state history");
    }
    
    g_logger.info("[LendingContractManager] Created lending contract: " + contract_id);
    
    return contract_id;
}

bool LendingContractManager::activateLoan(
    const std::string& contract_id,
    const std::string& funding_txid) {
    
    // Update state to active
    return updateLendingState(contract_id, "active", "", "{\"funding_txid\":\"" + funding_txid + "\"}");
}

bool LendingContractManager::recordPayment(
    const std::string& contract_id,
    uint32_t payment_number,
    const std::string& payment_txid,
    double amount_paid) {
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return false;
    }
    
    // Parse contract data to get repayment schedule
    // In production, use proper JSON parsing
    // For now, update contract_data with payment info
    
    std::ostringstream payment_data;
    payment_data << "{\"payment_number\":" << payment_number
                 << ",\"payment_txid\":\"" << payment_txid
                 << "\",\"amount_paid\":" << std::fixed << std::setprecision(8) << amount_paid
                 << ",\"paid_date\":" << std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() << "}";
    
    // Update contract data
    contract.contract_data += "\n" + payment_data.str();
    
    // Recalculate state hash
    std::string new_state_hash = calculateLendingStateHash(contract_id, "active", contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    // Update contract
    if (!db_.updateContract(contract_id, contract)) {
        return false;
    }
    
    // Record state history
    StateHistoryEntry history;
    history.contract_id = contract_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = contract.party_b_address; // Borrower makes payment
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

RepaymentEntry LendingContractManager::calculateNextPayment(const std::string& contract_id) const {
    RepaymentEntry entry;
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return entry;
    }
    
    // Get repayment schedule
    auto schedule = getRepaymentSchedule(contract_id);
    
    // Find first unpaid payment
    for (const auto& payment : schedule) {
        if (!payment.paid) {
            return payment;
        }
    }
    
    return entry; // All paid
}

std::vector<RepaymentEntry> LendingContractManager::getRepaymentSchedule(const std::string& contract_id) const {
    std::vector<RepaymentEntry> schedule;
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return schedule;
    }
    
    // Parse contract data to extract loan terms
    // In production, use proper JSON parsing
    // For now, generate schedule from contract creation time
    
    // Extract terms from contract_data (simplified - would use JSON parser in production)
    // Assume format: {"principal":100.0,"interest_rate":5.0,"term_months":12,...}
    
    // Generate schedule
    uint64_t start_date = std::chrono::duration_cast<std::chrono::seconds>(
        contract.created_at.time_since_epoch()).count();
    
    // For now, return empty schedule - full implementation would parse contract_data
    // and generate proper schedule
    
    return schedule;
}

bool LendingContractManager::isOverdue(const std::string& contract_id) const {
    auto next_payment = calculateNextPayment(contract_id);
    if (next_payment.payment_number == 0) {
        return false; // No payments or all paid
    }
    
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return (current_time > next_payment.due_date) && !next_payment.paid;
}

bool LendingContractManager::updateLendingState(
    const std::string& contract_id,
    const std::string& new_status,
    const std::string& transitioned_by,
    const std::string& transition_data) {
    
    if (!db_.isOpen()) {
        return false;
    }
    
    // Get current contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return false;
    }
    
    // Update status
    LendingStatus lending_status = (new_status == "pending") ? LendingStatus::PENDING :
                                   (new_status == "active") ? LendingStatus::ACTIVE :
                                   (new_status == "overdue") ? LendingStatus::OVERDUE :
                                   (new_status == "repaid") ? LendingStatus::REPAID :
                                   (new_status == "defaulted") ? LendingStatus::DEFAULTED :
                                   LendingStatus::CANCELLED;
    
    contract.status = lendingStatusToContractStatus(lending_status);
    contract.updated_at = std::chrono::system_clock::now();
    
    // Update contract data if provided
    if (!transition_data.empty()) {
        contract.contract_data += "\n" + transition_data;
    }
    
    // Recalculate state hash
    std::string new_state_hash = calculateLendingStateHash(contract_id, new_status, contract.contract_data);
    contract.state_hash = new_state_hash;
    
    // Update contract
    if (!db_.updateContract(contract_id, contract)) {
        return false;
    }
    
    // Record state history
    StateHistoryEntry history;
    history.contract_id = contract_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    
    TransitionType transition_type = TransitionType::UPDATE;
    if (new_status == "repaid") {
        transition_type = TransitionType::SETTLE;
    } else if (new_status == "defaulted" || new_status == "cancelled") {
        transition_type = TransitionType::CANCEL;
    }
    
    history.transition_type = transition_type;
    history.transitioned_by = transitioned_by.empty() ? contract.party_a_address : transitioned_by;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

std::optional<std::vector<uint8_t>> LendingContractManager::createCommitment(const std::string& contract_id) {
    if (!db_.isOpen()) {
        return std::nullopt;
    }
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return std::nullopt;
    }
    
    // Get state history for Merkle root
    std::vector<StateHistoryEntry> history = db_.getStateHistory(contract_id);
    std::vector<std::string> state_hashes;
    for (const auto& entry : history) {
        state_hashes.push_back(entry.state_hash);
    }
    state_hashes.push_back(contract.state_hash);
    
    // Calculate Merkle root
    std::string merkle_root = CommitmentTransactionBuilder::calculateMerkleRoot(state_hashes);
    
    // Build commitment data
    CommitmentTransactionBuilder::CommitmentData commitment;
    commitment.version = 0x01;
    
    // Contract ID hash
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
        return std::nullopt;
    }
    
    // Update contract with Merkle root
    contract.merkle_root = merkle_root;
    db_.updateContract(contract_id, contract);
    
    return script;
}

bool LendingContractManager::recordCommitmentTransaction(
    const std::string& contract_id,
    const std::string& commitment_txid,
    uint32_t block_height,
    const std::string& block_hash) {
    
    if (!db_.isOpen()) {
        return false;
    }
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
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
    
    // Get commitment script
    auto script_opt = createCommitment(contract_id);
    if (script_opt) {
        std::ostringstream hex_oss;
        for (uint8_t byte : *script_opt) {
            hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        commitment.commitment_data = hex_oss.str();
    }
    
    // Add commitment
    if (!db_.addCommitment(commitment)) {
        return false;
    }
    
    // Update contract
    contract.commitment_txid = commitment_txid;
    db_.updateContract(contract_id, contract);
    
    return true;
}

bool LendingContractManager::getLendingContract(const std::string& contract_id, ContractState& out) const {
    return db_.getContract(contract_id, out);
}

std::vector<StateHistoryEntry> LendingContractManager::getLendingHistory(const std::string& contract_id) const {
    return db_.getStateHistory(contract_id);
}

bool LendingContractManager::verifyLendingState(const std::string& contract_id) const {
    if (!verifier_) {
        return false;
    }
    return verifier_->verifyContractState(contract_id);
}

StateVerifier::VerificationReport LendingContractManager::getVerificationReport(const std::string& contract_id) const {
    if (!verifier_) {
        StateVerifier::VerificationReport report;
        report.errors.push_back("Verifier not initialized");
        return report;
    }
    return verifier_->generateReport(contract_id);
}

// Helper functions
ContractStatus LendingContractManager::lendingStatusToContractStatus(LendingStatus status) const {
    switch (status) {
        case LendingStatus::PENDING: return ContractStatus::PENDING;
        case LendingStatus::ACTIVE: return ContractStatus::ACTIVE;
        case LendingStatus::OVERDUE: return ContractStatus::DISPUTED;
        case LendingStatus::REPAID: return ContractStatus::SETTLED;
        case LendingStatus::DEFAULTED: return ContractStatus::CANCELLED;
        case LendingStatus::CANCELLED: return ContractStatus::CANCELLED;
        default: return ContractStatus::PENDING;
    }
}

std::string LendingContractManager::buildLendingContractData(
    const std::string& lender_address,
    const std::string& borrower_address,
    double principal_amount,
    double interest_rate,
    uint32_t term_months,
    LendingType lending_type,
    const std::string& collateral_address,
    double collateral_amount) const {
    
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"lending\","
        << "\"lender\":\"" << lender_address << "\","
        << "\"borrower\":\"" << borrower_address << "\","
        << "\"principal\":" << std::fixed << std::setprecision(8) << principal_amount << ","
        << "\"interest_rate\":" << interest_rate << ","
        << "\"term_months\":" << term_months << ",";
    
    if (lending_type == LendingType::SIMPLE_INTEREST) {
        oss << "\"lending_type\":\"simple\",";
    } else if (lending_type == LendingType::COMPOUND_INTEREST) {
        oss << "\"lending_type\":\"compound\",";
    } else {
        oss << "\"lending_type\":\"collateralized\",";
    }
    
    if (!collateral_address.empty()) {
        oss << "\"collateral_address\":\"" << collateral_address << "\","
            << "\"collateral_amount\":" << std::fixed << std::setprecision(8) << collateral_amount;
    }
    
    oss << "}";
    
    return oss.str();
}

std::string LendingContractManager::calculateLendingStateHash(
    const std::string& contract_id,
    const std::string& status,
    const std::string& contract_data) const {
    
    std::ostringstream oss;
    oss << contract_id << status << contract_data;
    
    std::string state_string = oss.str();
    
    uint8_t hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(state_string.c_str()), 
                   state_string.length()).Finalize(hash);
    
    std::ostringstream hex_oss;
    for (int i = 0; i < 32; i++) {
        hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return hex_oss.str();
}

std::vector<RepaymentEntry> LendingContractManager::generateRepaymentSchedule(
    double principal,
    double annual_rate,
    uint32_t term_months,
    uint64_t start_date) const {
    
    std::vector<RepaymentEntry> schedule;
    
    double monthly_rate = annual_rate / 100.0 / 12.0;
    double monthly_payment = principal * (monthly_rate * std::pow(1 + monthly_rate, term_months)) /
                            (std::pow(1 + monthly_rate, term_months) - 1);
    
    double remaining_principal = principal;
    
    for (uint32_t i = 1; i <= term_months; i++) {
        RepaymentEntry entry;
        entry.payment_number = i;
        
        // Calculate due date (30 days per month)
        entry.due_date = start_date + (i * 30 * 24 * 3600);
        
        // Calculate interest portion
        entry.interest_amount = remaining_principal * monthly_rate;
        
        // Calculate principal portion
        entry.principal_amount = monthly_payment - entry.interest_amount;
        
        // Total payment
        entry.total_amount = monthly_payment;
        
        entry.paid = false;
        
        remaining_principal -= entry.principal_amount;
        
        schedule.push_back(entry);
    }
    
    return schedule;
}

double LendingContractManager::calculateSimpleInterestPayment(
    double principal,
    double annual_rate,
    uint32_t term_months) const {
    
    double total_interest = principal * (annual_rate / 100.0) * (term_months / 12.0);
    return (principal + total_interest) / term_months;
}

double LendingContractManager::calculateCompoundInterestPayment(
    double principal,
    double annual_rate,
    uint32_t term_months) const {
    
    double monthly_rate = annual_rate / 100.0 / 12.0;
    return principal * (monthly_rate * std::pow(1 + monthly_rate, term_months)) /
           (std::pow(1 + monthly_rate, term_months) - 1);
}

} // namespace contracts
} // namespace dinero

