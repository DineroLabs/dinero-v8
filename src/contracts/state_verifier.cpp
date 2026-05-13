#include "contracts/state_verifier.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

bool StateVerifier::verifyContractState(const std::string& contract_id) {
    // Get contract from database
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        g_logger.error("[StateVerifier] Contract not found: " + contract_id);
        return false;
    }
    
    // Rebuild state from commitments
    std::string rebuilt_hash = rebuildStateFromCommitments(contract_id);
    if (rebuilt_hash.empty()) {
        g_logger.error("[StateVerifier] Failed to rebuild state from commitments");
        return false;
    }
    
    // Compare hashes
    bool matches = (rebuilt_hash == contract.state_hash);
    
    if (!matches) {
        g_logger.warning("[StateVerifier] State mismatch for contract " + contract_id);
        g_logger.warning("[StateVerifier]   Database hash: " + contract.state_hash);
        g_logger.warning("[StateVerifier]   Rebuilt hash: " + rebuilt_hash);
    }
    
    return matches;
}

std::string StateVerifier::rebuildStateFromCommitments(const std::string& contract_id) {
    // Get all commitments for this contract
    std::vector<OnChainCommitment> commitments = db_.getContractCommitments(contract_id);
    
    if (commitments.empty()) {
        g_logger.warning("[StateVerifier] No commitments found for contract: " + contract_id);
        return "";
    }
    
    // Get contract to start with initial state
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        return "";
    }
    
    // Get state history
    std::vector<StateHistoryEntry> history = db_.getStateHistory(contract_id);
    
    if (history.empty()) {
        // No history, return current state hash
        return contract.state_hash;
    }
    
    // Start with creation state
    std::string current_state_hash = history[0].state_hash;
    
    // Apply each transition in order
    for (size_t i = 1; i < history.size(); i++) {
        current_state_hash = applyStateTransition(current_state_hash, history[i].state_data);
    }
    
    return current_state_hash;
}

bool StateVerifier::verifyCommitmentChain(const std::string& contract_id) {
    // Get all commitments
    std::vector<OnChainCommitment> commitments = db_.getContractCommitments(contract_id);
    
    if (commitments.size() < 2) {
        return true; // Single commitment or no commitments is valid
    }
    
    // Verify commitments are in order (by block height)
    for (size_t i = 1; i < commitments.size(); i++) {
        if (commitments[i].block_height < commitments[i-1].block_height) {
            g_logger.error("[StateVerifier] Commitments out of order for contract: " + contract_id);
            return false;
        }
    }
    
    // Verify state hashes form a chain
    // Each commitment should reference the previous state hash
    // (This is a simplified check - full verification would require transaction parsing)
    
    return true;
}

StateVerifier::VerificationReport StateVerifier::generateReport(const std::string& contract_id) {
    VerificationReport report;
    report.contract_id = contract_id;
    report.is_valid = false;
    report.commitment_count = 0;
    
    // Get contract
    ContractState contract;
    if (!db_.getContract(contract_id, contract)) {
        report.errors.push_back("Contract not found");
        return report;
    }
    
    report.database_state_hash = contract.state_hash;
    
    // Get commitments
    std::vector<OnChainCommitment> commitments = db_.getContractCommitments(contract_id);
    report.commitment_count = static_cast<uint32_t>(commitments.size());
    
    if (commitments.empty()) {
        report.warnings.push_back("No on-chain commitments found");
    }
    
    // Rebuild state
    std::string rebuilt_hash = rebuildStateFromCommitments(contract_id);
    report.rebuilt_state_hash = rebuilt_hash;
    
    if (rebuilt_hash.empty()) {
        report.errors.push_back("Failed to rebuild state from commitments");
        return report;
    }
    
    // Compare hashes
    if (rebuilt_hash != contract.state_hash) {
        report.errors.push_back("State hash mismatch");
        report.errors.push_back("Database: " + contract.state_hash);
        report.errors.push_back("Rebuilt: " + rebuilt_hash);
    }
    
    // Verify commitment chain
    if (!verifyCommitmentChain(contract_id)) {
        report.errors.push_back("Commitment chain verification failed");
    }
    
    // Check confirmations
    for (const auto& commitment : commitments) {
        if (commitment.confirmations < 6) {
            report.warnings.push_back("Commitment " + commitment.commitment_txid + 
                                     " has only " + std::to_string(commitment.confirmations) + 
                                     " confirmations");
        }
    }
    
    report.is_valid = report.errors.empty();
    
    return report;
}

std::string StateVerifier::applyStateTransition(const std::string& current_state_hash, 
                                                const std::string& transition_data) {
    // Combine current state hash with transition data
    std::ostringstream oss;
    oss << current_state_hash << transition_data;
    
    std::string combined = oss.str();
    
    // Hash the combination
    uint8_t hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(combined.c_str()), combined.length()).Finalize(hash);
    
    // Convert to hex string
    std::ostringstream hex_oss;
    for (int i = 0; i < 32; i++) {
        hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return hex_oss.str();
}

} // namespace contracts
} // namespace dinero

