#pragma once

#include "contracts/contract_state_db.h"
#include "contracts/commitment_builder.h"
#include "contracts/state_verifier.h"
#include <string>
#include <memory>
#include <optional>
#include <cstdint>
#include <chrono>
#include <vector>
#include <map>

namespace dinero {
namespace contracts {

// Proposal types
enum class ProposalType {
    SPENDING = 1,           // Spending proposal (transfer funds)
    PARAMETER_CHANGE = 2,   // Change DAO parameters
    MEMBERSHIP = 3,         // Add/remove members
    EMERGENCY = 4           // Emergency proposal (fast-track)
};

// Proposal status
enum class ProposalStatus {
    DRAFT = 1,              // Draft (not yet submitted)
    ACTIVE = 2,             // Active voting period
    PASSED = 3,             // Passed (voting successful)
    REJECTED = 4,           // Rejected (voting failed)
    EXECUTED = 5,           // Executed (action taken)
    EXPIRED = 6             // Expired (timeout)
};

// Vote choice
enum class VoteChoice {
    YES = 1,
    NO = 2,
    ABSTAIN = 3
};

// Vote entry
struct VoteEntry {
    std::string voter_address;
    VoteChoice choice;
    uint64_t voting_power;      // Voting power (e.g., token amount)
    uint64_t timestamp;
    std::string signature;      // Signature proving vote
    
    VoteEntry() : choice(VoteChoice::ABSTAIN), voting_power(0), timestamp(0) {}
};

// Proposal structure
struct Proposal {
    std::string proposal_id;
    std::string dao_id;
    ProposalType type;
    ProposalStatus status;
    std::string proposer_address;
    std::string title;
    std::string description;
    std::string proposal_data;  // JSON with proposal details
    uint64_t voting_start;
    uint64_t voting_end;
    uint64_t quorum_threshold;   // Minimum voting power required
    double approval_threshold;   // Percentage needed to pass (e.g., 0.51 = 51%)
    uint64_t created_at;
    uint64_t executed_at;
    std::string execution_txid;
    
    // Voting results
    uint64_t yes_votes;
    uint64_t no_votes;
    uint64_t abstain_votes;
    uint64_t total_voting_power;
    
    Proposal() : type(ProposalType::SPENDING), status(ProposalStatus::DRAFT),
                 voting_start(0), voting_end(0), quorum_threshold(0),
                 approval_threshold(0.51), created_at(0), executed_at(0),
                 yes_votes(0), no_votes(0), abstain_votes(0), total_voting_power(0) {}
};

// DAO member
struct DAOMember {
    std::string address;
    uint64_t voting_power;
    uint64_t joined_at;
    bool is_active;
    
    DAOMember() : voting_power(0), joined_at(0), is_active(true) {}
};

// DAO Governance Contract Manager
class DAOGovernanceManager {
public:
    DAOGovernanceManager(ContractStateDB& db);
    
    // DAO Management
    std::optional<std::string> createDAO(
        const std::string& creator_address,
        const std::string& dao_name,
        const std::string& dao_description,
        uint64_t quorum_threshold,
        double approval_threshold,
        const std::vector<std::string>& initial_members = {}
    );
    
    bool addMember(
        const std::string& dao_id,
        const std::string& member_address,
        uint64_t voting_power
    );
    
    bool removeMember(
        const std::string& dao_id,
        const std::string& member_address
    );
    
    std::vector<DAOMember> getMembers(const std::string& dao_id) const;
    
    // Proposal Management
    std::optional<std::string> createProposal(
        const std::string& dao_id,
        const std::string& proposer_address,
        ProposalType type,
        const std::string& title,
        const std::string& description,
        const std::string& proposal_data,
        uint64_t voting_duration_seconds = 604800  // Default 7 days
    );
    
    bool submitProposal(const std::string& proposal_id);
    
    bool voteOnProposal(
        const std::string& proposal_id,
        const std::string& voter_address,
        VoteChoice choice,
        uint64_t voting_power,
        const std::string& signature = ""
    );
    
    bool executeProposal(
        const std::string& proposal_id,
        const std::string& executor_address,
        const std::string& execution_txid
    );
    
    Proposal getProposal(const std::string& proposal_id) const;
    
    std::vector<Proposal> getDAOProposals(const std::string& dao_id) const;
    
    std::vector<VoteEntry> getProposalVotes(const std::string& proposal_id) const;
    
    // Proposal status checks
    bool checkProposalStatus(const std::string& proposal_id);
    
    bool isProposalPassed(const std::string& proposal_id) const;
    
    bool canExecuteProposal(const std::string& proposal_id) const;
    
    // State management
    bool updateDAOState(
        const std::string& dao_id,
        const std::string& transition_data
    );
    
    std::optional<std::vector<uint8_t>> createCommitment(const std::string& dao_id);
    
    bool recordCommitmentTransaction(
        const std::string& dao_id,
        const std::string& commitment_txid,
        uint32_t block_height = 0,
        const std::string& block_hash = ""
    );
    
    bool getDAOContract(const std::string& dao_id, ContractState& out) const;
    
    std::vector<StateHistoryEntry> getDAOHistory(const std::string& dao_id) const;
    
    bool verifyDAOState(const std::string& dao_id) const;
    
    StateVerifier::VerificationReport getVerificationReport(const std::string& dao_id) const;
    
private:
    ContractStateDB& db_;
    std::unique_ptr<StateVerifier> verifier_;
    
    // Helper: Build DAO contract data JSON
    std::string buildDAOContractData(
        const std::string& dao_name,
        const std::string& dao_description,
        uint64_t quorum_threshold,
        double approval_threshold,
        const std::vector<std::string>& members
    ) const;
    
    // Helper: Build proposal data JSON
    std::string buildProposalData(
        const Proposal& proposal,
        const std::vector<VoteEntry>& votes
    ) const;
    
    // Helper: Calculate state hash
    std::string calculateDAOStateHash(
        const std::string& dao_id,
        const std::string& contract_data
    ) const;
    
    // Helper: Calculate voting results
    void calculateVotingResults(
        const std::vector<VoteEntry>& votes,
        uint64_t& yes_votes,
        uint64_t& no_votes,
        uint64_t& abstain_votes,
        uint64_t& total_power
    ) const;
    
    // Helper: Check if proposal meets quorum
    bool meetsQuorum(uint64_t total_voting_power, uint64_t quorum_threshold) const;
    
    // Helper: Check if proposal meets approval threshold
    bool meetsApprovalThreshold(uint64_t yes_votes, uint64_t total_votes, double threshold) const;
};

}} // namespace dinero::contracts

