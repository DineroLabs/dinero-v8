#pragma once

#include <cstdint>
#include <string>

namespace dinero {

// Forward declarations
class CBlockIndex;
struct ChainParams;
struct ConsensusParams;

/**
 * @brief Dinero Algorithm State for difficulty calculations
 * 
 * Tracks the state needed for dynamic difficulty adjustments
 * based on total coins mined and mining phases.
 */
struct DineroAlgoState {
    uint64_t total_coins_mined{0};
    uint32_t current_height{0};
    uint32_t last_difficulty_adjustment{0};
    bool in_cpu_friendly_phase{false};
    bool in_halving_phase{false};
    
    // Phase transition tracking
    bool developer_fund_complete{false};
    bool cpu_friendly_complete{false};
    
    // Mining statistics
    uint32_t blocks_in_current_phase{0};
    uint64_t coins_in_current_phase{0};
};

/**
 * @brief Calculate next work required using Dinero Algorithm
 * 
 * This function implements the core Dinero difficulty adjustment algorithm:
 * - CPU-Friendly Phase: Easy difficulty for community mining
 * - Halving Phase: Bitcoin-level difficulty with regular adjustments
 * 
 * @param tip Current blockchain tip
 * @param cp Chain parameters
 * @param state Algorithm state (updated by this function)
 * @return uint32_t Compact difficulty target (nBits)
 */
uint32_t CalcNextWorkRequired(const CBlockIndex& tip, const ChainParams& cp, DineroAlgoState& state);

/**
 * @brief Update algorithm state based on new block
 * 
 * @param state Algorithm state to update
 * @param block_reward Reward for the new block
 * @param height New block height
 */
void UpdateAlgoState(DineroAlgoState& state, uint64_t block_reward, uint32_t height);

/**
 * @brief Get current mining phase description
 * 
 * @param state Current algorithm state
 * @return std::string Human-readable phase description
 */
std::string GetMiningPhaseDescription(const DineroAlgoState& state);

/**
 * @brief Check if difficulty should be adjusted
 * 
 * @param state Current algorithm state
 * @param height Current block height
 * @return bool True if difficulty adjustment is needed
 */
bool ShouldAdjustDifficulty(const DineroAlgoState& state, uint32_t height);

/**
 * @brief Select difficulty bits based on total mined supply
 * 
 * Returns compact "bits" to use given total mined supply (in una).
 * Schedule:
 *   [0, devFundEndSats)      -> nInitialDifficultyBits
 *   [devFundEndSats, phase2) -> easyBits (CPU-friendly)
 *   [phase2, +inf)           -> normalBits (fallback to nInitialDifficultyBits if 0)
 * 
 * @param totalMinedSats Total mined supply in una
 * @param params Consensus parameters containing difficulty schedule
 * @return uint32_t Compact difficulty bits
 */
uint32_t SelectDifficultyForSupply(uint64_t totalMinedSats, const ConsensusParams& params);

} // namespace dinero
