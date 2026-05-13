#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace dinero {
namespace contracts {

// Commitment transaction builder
// Creates OP_RETURN transactions for on-chain state commitments
class CommitmentTransactionBuilder {
public:
    // Commitment data structure
    struct CommitmentData {
        uint8_t version;              // Protocol version (0x01)
        std::string contract_id;     // 32 bytes: SHA256 hash of contract creation data
        std::string state_hash;      // 32 bytes: SHA256 of current state JSON
        std::string merkle_root;     // 32 bytes: Merkle root of state tree
        uint64_t nonce;              // 8 bytes: Random nonce for uniqueness
        
        CommitmentData() : version(0x01), nonce(0) {}
    };
    
    // Build OP_RETURN script for commitment
    // Returns: Script bytes (OP_RETURN + commitment data)
    static std::vector<uint8_t> buildCommitmentScript(const CommitmentData& data);
    
    // Parse OP_RETURN data from transaction
    // Returns: CommitmentData if valid, empty if invalid
    static bool parseCommitmentScript(const std::vector<uint8_t>& script, CommitmentData& out);
    
    // Calculate Merkle root from state history
    // Takes list of state hashes and computes Merkle root
    static std::string calculateMerkleRoot(const std::vector<std::string>& state_hashes);
    
    // Generate random nonce
    static uint64_t generateNonce();
    
    // Validate commitment data
    static bool validateCommitment(const CommitmentData& data);
    
private:
    // Helper: Encode commitment data to binary format
    static std::vector<uint8_t> encodeCommitment(const CommitmentData& data);
    
    // Helper: Decode commitment data from binary format
    static bool decodeCommitment(const std::vector<uint8_t>& data, CommitmentData& out);
    
    // Helper: Hash two values together (Merkle tree)
    static std::string hashPair(const std::string& left, const std::string& right);
};

}} // namespace dinero::contracts

