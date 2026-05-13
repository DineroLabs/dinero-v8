#pragma once

#include "contracts/contract_state_db.h"
#include "contracts/commitment_builder.h"
#include <string>
#include <vector>

namespace dinero {
namespace contracts {

// State verification utility
// Verifies contract state by rebuilding from on-chain commitments
class StateVerifier {
public:
    StateVerifier(ContractStateDB& db) : db_(db) {}
    
    // Verify contract state by rebuilding from commitments
    // Returns: true if state matches database, false otherwise
    bool verifyContractState(const std::string& contract_id);
    
    // Rebuild state from on-chain commitments
    // Returns: Rebuilt state hash, or empty string on failure
    std::string rebuildStateFromCommitments(const std::string& contract_id);
    
    // Verify commitment chain integrity
    // Checks that all commitments form a valid chain
    bool verifyCommitmentChain(const std::string& contract_id);
    
    // Get verification report
    struct VerificationReport {
        bool is_valid;
        std::string contract_id;
        std::string database_state_hash;
        std::string rebuilt_state_hash;
        uint32_t commitment_count;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };
    
    VerificationReport generateReport(const std::string& contract_id);
    
private:
    ContractStateDB& db_;
    
    // Helper: Apply state transition
    std::string applyStateTransition(const std::string& current_state_hash, 
                                     const std::string& transition_data);
};

}} // namespace dinero::contracts

