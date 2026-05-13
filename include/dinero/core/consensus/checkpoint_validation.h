#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace dinero {

// Forward declarations
struct ConsensusParams;

/**
 * @brief Checkpoint validation for hardcoded block headers
 * 
 * Enforces that blocks at specific heights match hardcoded header hashes.
 * This prevents deep reorgs and alternative histories at known heights.
 */
class CheckpointValidator {
public:
    /**
     * @brief Validate block header against hardcoded checkpoints
     * 
     * @param blockHeight Block height being validated
     * @param headerHash Block header hash
     * @param consensus Consensus parameters with checkpoints
     * @param errorMsg Output parameter for error description
     * @return bool True if checkpoint validation passes
     */
    static bool ValidateCheckpoint(
        uint32_t blockHeight,
        const std::vector<uint8_t>& headerHash,
        const ConsensusParams& consensus,
        std::string& errorMsg
    );
    
    /**
     * @brief Check if block height has a hardcoded checkpoint
     * 
     * @param blockHeight Block height to check
     * @param consensus Consensus parameters
     * @return bool True if this height has a checkpoint
     */
    static bool HasCheckpoint(uint32_t blockHeight, const ConsensusParams& consensus);
    
    /**
     * @brief Get checkpoint hash for a specific height
     * 
     * @param blockHeight Block height
     * @param consensus Consensus parameters
     * @return std::vector<uint8_t> Checkpoint hash (empty if not found)
     */
    static std::vector<uint8_t> GetCheckpointHash(uint32_t blockHeight, const ConsensusParams& consensus);

private:
    /**
     * @brief Convert bytes to hex string for error messages
     */
    static std::string BytesToHex(const std::vector<uint8_t>& bytes);
};

} // namespace dinero
