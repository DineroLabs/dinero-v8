#include "contracts/commitment_builder.h"
#include "crypto/sha256.h"
#include "common/logger.h"
#include <random>
#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace dinero::crypto;

namespace dinero {
namespace contracts {

// OP_RETURN opcode
constexpr uint8_t OP_RETURN = 0x6a;

std::vector<uint8_t> CommitmentTransactionBuilder::buildCommitmentScript(const CommitmentData& data) {
    std::vector<uint8_t> script;
    
    // Validate commitment data
    if (!validateCommitment(data)) {
        g_logger.error("[CommitmentBuilder] Invalid commitment data");
        return script;
    }
    
    // Encode commitment data
    std::vector<uint8_t> commitment_bytes = encodeCommitment(data);
    
    // OP_RETURN script format: OP_RETURN <length> <data>
    script.push_back(OP_RETURN);
    
    // Push data length (compact encoding for small values)
    if (commitment_bytes.size() <= 75) {
        script.push_back(static_cast<uint8_t>(commitment_bytes.size()));
    } else if (commitment_bytes.size() <= 255) {
        script.push_back(0x4c); // OP_PUSHDATA1
        script.push_back(static_cast<uint8_t>(commitment_bytes.size()));
    } else if (commitment_bytes.size() <= 65535) {
        script.push_back(0x4d); // OP_PUSHDATA2
        script.push_back(static_cast<uint8_t>(commitment_bytes.size() & 0xff));
        script.push_back(static_cast<uint8_t>((commitment_bytes.size() >> 8) & 0xff));
    } else {
        g_logger.error("[CommitmentBuilder] Commitment data too large: " + std::to_string(commitment_bytes.size()));
        return script;
    }
    
    // Append commitment data
    script.insert(script.end(), commitment_bytes.begin(), commitment_bytes.end());
    
    return script;
}

bool CommitmentTransactionBuilder::parseCommitmentScript(const std::vector<uint8_t>& script, CommitmentData& out) {
    if (script.empty() || script[0] != OP_RETURN) {
        return false;
    }
    
    size_t pos = 1;
    size_t data_len = 0;
    
    // Parse length encoding
    if (pos >= script.size()) return false;
    
    uint8_t len_byte = script[pos++];
    if (len_byte <= 75) {
        data_len = len_byte;
    } else if (len_byte == 0x4c) { // OP_PUSHDATA1
        if (pos >= script.size()) return false;
        data_len = script[pos++];
    } else if (len_byte == 0x4d) { // OP_PUSHDATA2
        if (pos + 1 >= script.size()) return false;
        data_len = script[pos] | (script[pos + 1] << 8);
        pos += 2;
    } else {
        return false;
    }
    
    // Extract commitment data
    if (pos + data_len > script.size()) return false;
    
    std::vector<uint8_t> commitment_bytes(script.begin() + pos, script.begin() + pos + data_len);
    
    // Decode commitment data
    return decodeCommitment(commitment_bytes, out);
}

std::string CommitmentTransactionBuilder::calculateMerkleRoot(const std::vector<std::string>& state_hashes) {
    if (state_hashes.empty()) {
        return "";
    }
    
    if (state_hashes.size() == 1) {
        return state_hashes[0];
    }
    
    // Build Merkle tree bottom-up
    std::vector<std::string> current_level = state_hashes;
    
    while (current_level.size() > 1) {
        std::vector<std::string> next_level;
        
        // Process pairs
        for (size_t i = 0; i < current_level.size(); i += 2) {
            if (i + 1 < current_level.size()) {
                // Hash pair
                next_level.push_back(hashPair(current_level[i], current_level[i + 1]));
            } else {
                // Odd number: duplicate last element
                next_level.push_back(hashPair(current_level[i], current_level[i]));
            }
        }
        
        current_level = next_level;
    }
    
    return current_level[0];
}

uint64_t CommitmentTransactionBuilder::generateNonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    return dis(gen);
}

bool CommitmentTransactionBuilder::validateCommitment(const CommitmentData& data) {
    // Validate version
    if (data.version == 0 || data.version > 0xff) {
        return false;
    }
    
    // Validate contract_id (should be 32 bytes hex = 64 hex chars)
    if (data.contract_id.length() != 64) {
        return false;
    }
    
    // Validate state_hash (should be 32 bytes hex = 64 hex chars)
    if (data.state_hash.length() != 64) {
        return false;
    }
    
    // Validate merkle_root (should be 32 bytes hex = 64 hex chars)
    if (data.merkle_root.length() != 64) {
        return false;
    }
    
    return true;
}

std::vector<uint8_t> CommitmentTransactionBuilder::encodeCommitment(const CommitmentData& data) {
    std::vector<uint8_t> result;
    result.reserve(105); // version(1) + contract_id(32) + state_hash(32) + merkle_root(32) + nonce(8)
    
    // Version (1 byte)
    result.push_back(data.version);
    
    // Contract ID (32 bytes) - convert hex to bytes
    for (size_t i = 0; i < data.contract_id.length(); i += 2) {
        std::string byte_str = data.contract_id.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16));
        result.push_back(byte);
    }
    
    // State hash (32 bytes) - convert hex to bytes
    for (size_t i = 0; i < data.state_hash.length(); i += 2) {
        std::string byte_str = data.state_hash.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16));
        result.push_back(byte);
    }
    
    // Merkle root (32 bytes) - convert hex to bytes
    for (size_t i = 0; i < data.merkle_root.length(); i += 2) {
        std::string byte_str = data.merkle_root.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16));
        result.push_back(byte);
    }
    
    // Nonce (8 bytes, little-endian)
    uint64_t nonce = data.nonce;
    for (int i = 0; i < 8; i++) {
        result.push_back(static_cast<uint8_t>(nonce & 0xff));
        nonce >>= 8;
    }
    
    return result;
}

bool CommitmentTransactionBuilder::decodeCommitment(const std::vector<uint8_t>& data, CommitmentData& out) {
    // Expected size: 1 + 32 + 32 + 32 + 8 = 105 bytes
    if (data.size() != 105) {
        return false;
    }
    
    size_t pos = 0;
    
    // Version (1 byte)
    out.version = data[pos++];
    
    // Contract ID (32 bytes) - convert to hex
    std::ostringstream contract_id_oss;
    for (int i = 0; i < 32; i++) {
        contract_id_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[pos++]);
    }
    out.contract_id = contract_id_oss.str();
    
    // State hash (32 bytes) - convert to hex
    std::ostringstream state_hash_oss;
    for (int i = 0; i < 32; i++) {
        state_hash_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[pos++]);
    }
    out.state_hash = state_hash_oss.str();
    
    // Merkle root (32 bytes) - convert to hex
    std::ostringstream merkle_root_oss;
    for (int i = 0; i < 32; i++) {
        merkle_root_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[pos++]);
    }
    out.merkle_root = merkle_root_oss.str();
    
    // Nonce (8 bytes, little-endian)
    out.nonce = 0;
    for (int i = 0; i < 8; i++) {
        out.nonce |= (static_cast<uint64_t>(data[pos++]) << (i * 8));
    }
    
    return true;
}

std::string CommitmentTransactionBuilder::hashPair(const std::string& left, const std::string& right) {
    // Convert hex strings to bytes
    std::vector<uint8_t> left_bytes, right_bytes;
    
    for (size_t i = 0; i < left.length(); i += 2) {
        std::string byte_str = left.substr(i, 2);
        left_bytes.push_back(static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16)));
    }
    
    for (size_t i = 0; i < right.length(); i += 2) {
        std::string byte_str = right.substr(i, 2);
        right_bytes.push_back(static_cast<uint8_t>(strtol(byte_str.c_str(), nullptr, 16)));
    }
    
    // Concatenate left + right
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), left_bytes.begin(), left_bytes.end());
    combined.insert(combined.end(), right_bytes.begin(), right_bytes.end());
    
    // Hash with SHA256
    uint8_t hash[32];
    CSHA256().Write(combined.data(), combined.size()).Finalize(hash);
    
    // Convert to hex string
    std::ostringstream oss;
    for (int i = 0; i < 32; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return oss.str();
}

} // namespace contracts
} // namespace dinero

