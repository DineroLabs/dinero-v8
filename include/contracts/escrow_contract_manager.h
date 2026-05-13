#pragma once

#include "contracts/contract_state_db.h"
#include "contracts/commitment_builder.h"
#include "contracts/state_verifier.h"
#include "p2p/escrow_manager.h"
#include <string>
#include <memory>
#include <optional>

namespace dinero {
namespace contracts {

// Escrow contract manager with on-chain commitments
// Bridges existing EscrowManager with new ContractStateDB infrastructure
class EscrowContractManager {
public:
    EscrowContractManager(const std::string& db_path);
    ~EscrowContractManager();
    
    // Initialize database connection
    bool initialize();
    
    // Create escrow contract with state tracking
    // Returns: contract_id if successful
    std::optional<std::string> createEscrowContract(
        const std::string& seller_address,
        const std::string& buyer_address,
        const std::string& mediator_address,
        double amount,
        uint64_t duration_seconds,
        const std::string& offer_id
    );
    
    // Update escrow state (e.g., accept, release, refund)
    // Creates commitment transaction automatically
    bool updateEscrowState(
        const std::string& contract_id,
        const std::string& new_status,
        const std::string& transitioned_by,
        const std::string& transition_data = ""
    );
    
    // Create commitment transaction for current state
    // Returns: Commitment script bytes (ready for OP_RETURN)
    std::optional<std::vector<uint8_t>> createCommitment(
        const std::string& contract_id
    );
    
    // Record commitment transaction after broadcast
    bool recordCommitmentTransaction(
        const std::string& contract_id,
        const std::string& commitment_txid,
        uint32_t block_height = 0,
        const std::string& block_hash = ""
    );
    
    // Get escrow contract state
    bool getEscrowContract(const std::string& contract_id, ContractState& out) const;
    
    // Get escrow state history
    std::vector<StateHistoryEntry> getEscrowHistory(const std::string& contract_id) const;
    
    // Verify escrow state
    bool verifyEscrowState(const std::string& contract_id) const;
    
    // Get verification report
    StateVerifier::VerificationReport getVerificationReport(const std::string& contract_id) const;
    
private:
    std::unique_ptr<ContractStateDB> db_;
    std::unique_ptr<StateVerifier> verifier_;
    std::string db_path_;
    
    // Helper: Convert escrow status to contract status
    ContractStatus escrowStatusToContractStatus(const std::string& escrow_status) const;
    
    // Helper: Build contract data JSON from escrow info
    std::string buildEscrowContractData(
        const std::string& seller_address,
        const std::string& buyer_address,
        const std::string& mediator_address,
        double amount,
        uint64_t duration_seconds,
        const std::string& offer_id
    ) const;
    
    // Helper: Calculate state hash for escrow
    std::string calculateEscrowStateHash(
        const std::string& contract_id,
        const std::string& status,
        const std::string& contract_data
    ) const;
};

}} // namespace dinero::contracts

