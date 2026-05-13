#include "contracts/dao_governance_manager.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

DAOGovernanceManager::DAOGovernanceManager(ContractStateDB& db)
    : db_(db) {
    verifier_ = std::make_unique<StateVerifier>(db_);
}

std::optional<std::string> DAOGovernanceManager::createDAO(
    const std::string& creator_address,
    const std::string& dao_name,
    const std::string& dao_description,
    uint64_t quorum_threshold,
    double approval_threshold,
    const std::vector<std::string>& initial_members) {
    
    if (!db_.isOpen()) {
        g_logger.error("[DAOGovernanceManager] Database not open");
        return std::nullopt;
    }
    
    // Validate parameters
    if (dao_name.empty() || approval_threshold <= 0.0 || approval_threshold > 1.0) {
        g_logger.error("[DAOGovernanceManager] Invalid DAO parameters");
        return std::nullopt;
    }
    
    // Generate DAO ID
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    std::ostringstream oss;
    oss << "dao_" << std::hex << timestamp;
    std::string dao_id = oss.str();
    
    // Build contract data
    std::string contract_data = buildDAOContractData(
        dao_name, dao_description, quorum_threshold,
        approval_threshold, initial_members
    );
    
    // Calculate initial state hash
    std::string state_hash = calculateDAOStateHash(dao_id, contract_data);
    
    // Create contract state
    ContractState contract;
    contract.contract_id = dao_id;
    contract.contract_type = ContractType::DAO_GOVERNANCE;
    contract.state_hash = state_hash;
    contract.status = ContractStatus::ACTIVE; // DAO is active immediately
    contract.contract_data = contract_data;
    contract.party_a_address = creator_address;
    contract.party_b_address = ""; // Not applicable for DAO
    contract.mediator_address = ""; // No mediator
    
    auto now_time = std::chrono::system_clock::now();
    contract.created_at = now_time;
    contract.updated_at = now_time;
    
    // Create contract in database
    if (!db_.createContract(contract)) {
        g_logger.error("[DAOGovernanceManager] Failed to create DAO contract");
        return std::nullopt;
    }
    
    // Record initial state history
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = state_hash;
    history.commitment_txid = "";
    history.state_data = contract_data;
    history.transition_type = TransitionType::CREATE;
    history.transitioned_by = creator_address;
    history.block_height = 0;
    history.timestamp = now_time;
    
    db_.addStateHistory(history);
    
    g_logger.info("[DAOGovernanceManager] Created DAO: " + dao_id + " (" + dao_name + ")");
    
    return dao_id;
}

bool DAOGovernanceManager::addMember(
    const std::string& dao_id,
    const std::string& member_address,
    uint64_t voting_power) {
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    // Update contract data with new member
    // In production, use proper JSON parsing/updating
    std::ostringstream member_data;
    member_data << "\n{\"action\":\"add_member\",\"address\":\"" << member_address
                << "\",\"voting_power\":" << voting_power << "}";
    contract.contract_data += member_data.str();
    
    // Recalculate state hash
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    // Update contract
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    // Record state history
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = member_address;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

bool DAOGovernanceManager::removeMember(
    const std::string& dao_id,
    const std::string& member_address) {
    
    // Similar to addMember but with remove action
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    std::ostringstream member_data;
    member_data << "\n{\"action\":\"remove_member\",\"address\":\"" << member_address << "\"}";
    contract.contract_data += member_data.str();
    
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = member_address;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

std::vector<DAOMember> DAOGovernanceManager::getMembers(const std::string& dao_id) const {
    std::vector<DAOMember> members;
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return members;
    }
    
    // Parse contract_data to extract members
    // In production, use proper JSON parsing
    // For now, return empty - full implementation would parse JSON
    
    return members;
}

std::optional<std::string> DAOGovernanceManager::createProposal(
    const std::string& dao_id,
    const std::string& proposer_address,
    ProposalType type,
    const std::string& title,
    const std::string& description,
    const std::string& proposal_data,
    uint64_t voting_duration_seconds) {
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return std::nullopt;
    }
    
    // Generate proposal ID
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    std::ostringstream oss;
    oss << dao_id << "_proposal_" << std::hex << timestamp;
    std::string proposal_id = oss.str();
    
    // Create proposal
    Proposal proposal;
    proposal.proposal_id = proposal_id;
    proposal.dao_id = dao_id;
    proposal.type = type;
    proposal.status = ProposalStatus::DRAFT;
    proposal.proposer_address = proposer_address;
    proposal.title = title;
    proposal.description = description;
    proposal.proposal_data = proposal_data;
    
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    proposal.voting_start = 0; // Set when submitted
    proposal.voting_end = 0;
    proposal.created_at = current_time;
    
    // Get quorum and approval threshold from DAO contract data
    // In production, parse from contract_data JSON
    proposal.quorum_threshold = 1000; // Default
    proposal.approval_threshold = 0.51; // Default
    
    // Store proposal in contract data
    std::ostringstream proposal_json;
    proposal_json << "\n{\"action\":\"create_proposal\",\"proposal_id\":\"" << proposal_id
                  << "\",\"type\":" << static_cast<int>(type)
                  << ",\"title\":\"" << title << "\",\"status\":\"draft\"}";
    
    contract.contract_data += proposal_json.str();
    
    // Recalculate state hash
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    // Update contract
    if (!db_.updateContract(dao_id, contract)) {
        return std::nullopt;
    }
    
    // Record state history
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = proposer_address;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    g_logger.info("[DAOGovernanceManager] Created proposal: " + proposal_id);
    
    return proposal_id;
}

bool DAOGovernanceManager::submitProposal(const std::string& proposal_id) {
    // Extract DAO ID from proposal ID
    size_t pos = proposal_id.find("_proposal_");
    if (pos == std::string::npos) {
        return false;
    }
    std::string dao_id = proposal_id.substr(0, pos);
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    // Update proposal status to ACTIVE
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::ostringstream update_json;
    update_json << "\n{\"action\":\"submit_proposal\",\"proposal_id\":\"" << proposal_id
                << "\",\"status\":\"active\",\"voting_start\":" << current_time << "}";
    
    contract.contract_data += update_json.str();
    
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = "";
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

bool DAOGovernanceManager::voteOnProposal(
    const std::string& proposal_id,
    const std::string& voter_address,
    VoteChoice choice,
    uint64_t voting_power,
    const std::string& signature) {
    
    // Extract DAO ID
    size_t pos = proposal_id.find("_proposal_");
    if (pos == std::string::npos) {
        return false;
    }
    std::string dao_id = proposal_id.substr(0, pos);
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    // Record vote
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::ostringstream vote_json;
    vote_json << "\n{\"action\":\"vote\",\"proposal_id\":\"" << proposal_id
              << "\",\"voter\":\"" << voter_address
              << "\",\"choice\":" << static_cast<int>(choice)
              << ",\"voting_power\":" << voting_power
              << ",\"timestamp\":" << current_time << "}";
    
    contract.contract_data += vote_json.str();
    
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = voter_address;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    // Check proposal status after vote
    checkProposalStatus(proposal_id);
    
    return true;
}

bool DAOGovernanceManager::executeProposal(
    const std::string& proposal_id,
    const std::string& executor_address,
    const std::string& execution_txid) {
    
    // Extract DAO ID
    size_t pos = proposal_id.find("_proposal_");
    if (pos == std::string::npos) {
        return false;
    }
    std::string dao_id = proposal_id.substr(0, pos);
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    // Check if proposal can be executed
    if (!canExecuteProposal(proposal_id)) {
        return false;
    }
    
    // Record execution
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::ostringstream exec_json;
    exec_json << "\n{\"action\":\"execute_proposal\",\"proposal_id\":\"" << proposal_id
              << "\",\"executor\":\"" << executor_address
              << "\",\"execution_txid\":\"" << execution_txid
              << "\",\"executed_at\":" << current_time << "}";
    
    contract.contract_data += exec_json.str();
    
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::SETTLE;
    history.transitioned_by = executor_address;
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

Proposal DAOGovernanceManager::getProposal(const std::string& proposal_id) const {
    Proposal proposal;
    
    // Extract DAO ID
    size_t pos = proposal_id.find("_proposal_");
    if (pos == std::string::npos) {
        return proposal;
    }
    std::string dao_id = proposal_id.substr(0, pos);
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return proposal;
    }
    
    // Parse contract_data to extract proposal
    // In production, use proper JSON parsing
    proposal.proposal_id = proposal_id;
    proposal.dao_id = dao_id;
    
    return proposal;
}

std::vector<Proposal> DAOGovernanceManager::getDAOProposals(const std::string& dao_id) const {
    std::vector<Proposal> proposals;
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return proposals;
    }
    
    // Parse contract_data to extract all proposals
    // In production, use proper JSON parsing
    
    return proposals;
}

std::vector<VoteEntry> DAOGovernanceManager::getProposalVotes(const std::string& proposal_id) const {
    std::vector<VoteEntry> votes;
    
    // Extract DAO ID
    size_t pos = proposal_id.find("_proposal_");
    if (pos == std::string::npos) {
        return votes;
    }
    std::string dao_id = proposal_id.substr(0, pos);
    
    // Get DAO contract
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return votes;
    }
    
    // Parse contract_data to extract votes for this proposal
    // In production, use proper JSON parsing
    
    return votes;
}

bool DAOGovernanceManager::checkProposalStatus(const std::string& proposal_id) {
    Proposal proposal = getProposal(proposal_id);
    
    if (proposal.proposal_id.empty()) {
        return false;
    }
    
    // Get votes
    auto votes = getProposalVotes(proposal_id);
    
    // Calculate voting results
    uint64_t yes_votes = 0, no_votes = 0, abstain_votes = 0, total_power = 0;
    calculateVotingResults(votes, yes_votes, no_votes, abstain_votes, total_power);
    
    proposal.yes_votes = yes_votes;
    proposal.no_votes = no_votes;
    proposal.abstain_votes = abstain_votes;
    proposal.total_voting_power = total_power;
    
    // Check if voting period ended
    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    if (proposal.status == ProposalStatus::ACTIVE) {
        if (current_time >= proposal.voting_end) {
            // Voting period ended
            if (meetsQuorum(total_power, proposal.quorum_threshold) &&
                meetsApprovalThreshold(yes_votes, yes_votes + no_votes, proposal.approval_threshold)) {
                proposal.status = ProposalStatus::PASSED;
            } else {
                proposal.status = ProposalStatus::REJECTED;
            }
            
            // Update contract state
            std::string dao_id = proposal.dao_id;
            ContractState contract;
            if (db_.getContract(dao_id, contract)) {
                std::ostringstream status_json;
                status_json << "\n{\"action\":\"update_proposal_status\",\"proposal_id\":\"" << proposal_id
                            << "\",\"status\":\"" << (proposal.status == ProposalStatus::PASSED ? "passed" : "rejected") << "\"}";
                contract.contract_data += status_json.str();
                
                std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
                contract.state_hash = new_state_hash;
                contract.updated_at = std::chrono::system_clock::now();
                db_.updateContract(dao_id, contract);
            }
        }
    }
    
    return true;
}

bool DAOGovernanceManager::isProposalPassed(const std::string& proposal_id) const {
    Proposal proposal = getProposal(proposal_id);
    return proposal.status == ProposalStatus::PASSED;
}

bool DAOGovernanceManager::canExecuteProposal(const std::string& proposal_id) const {
    Proposal proposal = getProposal(proposal_id);
    
    if (proposal.status != ProposalStatus::PASSED) {
        return false;
    }
    
    if (proposal.executed_at > 0) {
        return false; // Already executed
    }
    
    return true;
}

bool DAOGovernanceManager::updateDAOState(
    const std::string& dao_id,
    const std::string& transition_data) {
    
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    contract.contract_data += "\n" + transition_data;
    std::string new_state_hash = calculateDAOStateHash(dao_id, contract.contract_data);
    contract.state_hash = new_state_hash;
    contract.updated_at = std::chrono::system_clock::now();
    
    if (!db_.updateContract(dao_id, contract)) {
        return false;
    }
    
    StateHistoryEntry history;
    history.contract_id = dao_id;
    history.state_hash = new_state_hash;
    history.commitment_txid = contract.commitment_txid;
    history.state_data = contract.contract_data;
    history.transition_type = TransitionType::UPDATE;
    history.transitioned_by = "";
    history.block_height = 0;
    history.timestamp = std::chrono::system_clock::now();
    
    db_.addStateHistory(history);
    
    return true;
}

std::optional<std::vector<uint8_t>> DAOGovernanceManager::createCommitment(const std::string& dao_id) {
    if (!db_.isOpen()) {
        return std::nullopt;
    }
    
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return std::nullopt;
    }
    
    std::vector<StateHistoryEntry> history = db_.getStateHistory(dao_id);
    std::vector<std::string> state_hashes;
    for (const auto& entry : history) {
        state_hashes.push_back(entry.state_hash);
    }
    state_hashes.push_back(contract.state_hash);
    
    std::string merkle_root = CommitmentTransactionBuilder::calculateMerkleRoot(state_hashes);
    
    CommitmentTransactionBuilder::CommitmentData commitment;
    commitment.version = 0x01;
    
    std::string contract_id_data = dao_id + contract.party_a_address + 
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
    
    std::vector<uint8_t> script = CommitmentTransactionBuilder::buildCommitmentScript(commitment);
    
    if (script.empty()) {
        return std::nullopt;
    }
    
    contract.merkle_root = merkle_root;
    db_.updateContract(dao_id, contract);
    
    return script;
}

bool DAOGovernanceManager::recordCommitmentTransaction(
    const std::string& dao_id,
    const std::string& commitment_txid,
    uint32_t block_height,
    const std::string& block_hash) {
    
    if (!db_.isOpen()) {
        return false;
    }
    
    ContractState contract;
    if (!db_.getContract(dao_id, contract)) {
        return false;
    }
    
    OnChainCommitment commitment;
    commitment.commitment_txid = commitment_txid;
    commitment.contract_id = dao_id;
    commitment.state_hash = contract.state_hash;
    commitment.merkle_root = contract.merkle_root;
    commitment.block_height = block_height;
    commitment.block_hash = block_hash;
    commitment.confirmations = 0;
    commitment.created_at = std::chrono::system_clock::now();
    
    auto script_opt = createCommitment(dao_id);
    if (script_opt) {
        std::ostringstream hex_oss;
        for (uint8_t byte : *script_opt) {
            hex_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        commitment.commitment_data = hex_oss.str();
    }
    
    if (!db_.addCommitment(commitment)) {
        return false;
    }
    
    contract.commitment_txid = commitment_txid;
    db_.updateContract(dao_id, contract);
    
    return true;
}

bool DAOGovernanceManager::getDAOContract(const std::string& dao_id, ContractState& out) const {
    return db_.getContract(dao_id, out);
}

std::vector<StateHistoryEntry> DAOGovernanceManager::getDAOHistory(const std::string& dao_id) const {
    return db_.getStateHistory(dao_id);
}

bool DAOGovernanceManager::verifyDAOState(const std::string& dao_id) const {
    if (!verifier_) {
        return false;
    }
    return verifier_->verifyContractState(dao_id);
}

StateVerifier::VerificationReport DAOGovernanceManager::getVerificationReport(const std::string& dao_id) const {
    if (!verifier_) {
        StateVerifier::VerificationReport report;
        report.errors.push_back("Verifier not initialized");
        return report;
    }
    return verifier_->generateReport(dao_id);
}

// Helper functions
std::string DAOGovernanceManager::buildDAOContractData(
    const std::string& dao_name,
    const std::string& dao_description,
    uint64_t quorum_threshold,
    double approval_threshold,
    const std::vector<std::string>& members) const {
    
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"dao\","
        << "\"name\":\"" << dao_name << "\","
        << "\"description\":\"" << dao_description << "\","
        << "\"quorum_threshold\":" << quorum_threshold << ","
        << "\"approval_threshold\":" << std::fixed << std::setprecision(2) << approval_threshold << ","
        << "\"members\":[";
    
    for (size_t i = 0; i < members.size(); i++) {
        if (i > 0) oss << ",";
        oss << "\"" << members[i] << "\"";
    }
    
    oss << "]}";
    
    return oss.str();
}

std::string DAOGovernanceManager::buildProposalData(
    const Proposal& proposal,
    const std::vector<VoteEntry>& votes) const {
    
    std::ostringstream oss;
    oss << "{"
        << "\"proposal_id\":\"" << proposal.proposal_id << "\","
        << "\"type\":" << static_cast<int>(proposal.type) << ","
        << "\"status\":" << static_cast<int>(proposal.status) << ","
        << "\"title\":\"" << proposal.title << "\","
        << "\"yes_votes\":" << proposal.yes_votes << ","
        << "\"no_votes\":" << proposal.no_votes << ","
        << "\"total_power\":" << proposal.total_voting_power
        << "}";
    
    return oss.str();
}

std::string DAOGovernanceManager::calculateDAOStateHash(
    const std::string& dao_id,
    const std::string& contract_data) const {
    
    std::ostringstream oss;
    oss << dao_id << contract_data;
    
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

void DAOGovernanceManager::calculateVotingResults(
    const std::vector<VoteEntry>& votes,
    uint64_t& yes_votes,
    uint64_t& no_votes,
    uint64_t& abstain_votes,
    uint64_t& total_power) const {
    
    yes_votes = 0;
    no_votes = 0;
    abstain_votes = 0;
    total_power = 0;
    
    for (const auto& vote : votes) {
        total_power += vote.voting_power;
        
        switch (vote.choice) {
            case VoteChoice::YES:
                yes_votes += vote.voting_power;
                break;
            case VoteChoice::NO:
                no_votes += vote.voting_power;
                break;
            case VoteChoice::ABSTAIN:
                abstain_votes += vote.voting_power;
                break;
        }
    }
}

bool DAOGovernanceManager::meetsQuorum(uint64_t total_voting_power, uint64_t quorum_threshold) const {
    return total_voting_power >= quorum_threshold;
}

bool DAOGovernanceManager::meetsApprovalThreshold(uint64_t yes_votes, uint64_t total_votes, double threshold) const {
    if (total_votes == 0) {
        return false;
    }
    
    double approval_ratio = static_cast<double>(yes_votes) / static_cast<double>(total_votes);
    return approval_ratio >= threshold;
}

} // namespace contracts
} // namespace dinero

