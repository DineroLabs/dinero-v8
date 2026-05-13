#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "primitives/uint256.h"

namespace dinero {

// Forward declarations
struct ConsensusParams;
struct NetworkParams;

/**
 * @brief Hash writer for deterministic consensus commitment computation
 */
class CHashWriter {
private:
    std::vector<uint8_t> buffer;
    
public:
    CHashWriter(int nType, int nVersion) {} // Compatibility with Bitcoin-style API
    
    template<typename T>
    CHashWriter& operator<<(const T& obj) {
        Serialize(obj);
        return *this;
    }
    
    uint256 GetHash();
    
private:
    void Serialize(uint8_t val);
    void Serialize(uint16_t val);
    void Serialize(uint32_t val);
    void Serialize(uint64_t val);
    void Serialize(const std::string& str);
    void Serialize(const std::vector<uint8_t>& vec);
};

/**
 * @brief Compute deterministic Chain Identity Commitment (CIC)
 * 
 * This function creates a cryptographic commitment to all consensus-critical
 * parameters, ensuring that any change to chain rules produces a different
 * commitment hash. This prevents accidental or malicious forks from being
 * accepted by nodes.
 * 
 * @param consensus Consensus parameters
 * @param network Network parameters  
 * @return uint256 32-byte commitment hash
 */
uint256 ComputeConsensusCommitment(const ConsensusParams& consensus, const NetworkParams& network);

/**
 * @brief Validate that computed CIC matches expected hardcoded value
 * 
 * @param computed The computed CIC from current parameters
 * @param expected The expected hardcoded CIC value
 * @return bool True if they match, false otherwise
 */
bool ValidateConsensusCommitment(const uint256& computed, const uint256& expected);

/**
 * @brief Get short prefix of CIC for logging and user display
 * 
 * @param cic The full 32-byte CIC
 * @return std::string First 12 characters of hex representation
 */
std::string GetCICPrefix(const uint256& cic);

} // namespace dinero
