#pragma once

#include <cstdint>
#include <string>
#include <atomic>

namespace dinero {

/**
 * SupplyTracker monitors the total supply of Dinero coins and manages phase transitions.
 * 
 * Phase 1 (CPU-Friendly): 0-20 million coins
 * - Difficulty: 0x207fffff (very easy for CPU mining)
 * - Reward: 100 DIN per block
 *
 * Phase 2 (Regular Halving): After 20 million coins, remaining supply
 * - Difficulty: 0x1d00ffff (Bitcoin mainnet level)
 * - Reward: Halves every 210,000 blocks (4 years)
 *
 * Total Supply: 99 million DIN
 */
class SupplyTracker {
public:
    SupplyTracker();
    ~SupplyTracker() = default;

    // Core functionality
    void addBlockReward(uint64_t reward);
    uint64_t getTotalCoinsMined() const;
    bool isEasyPhaseComplete() const;
    
    // Phase management
    bool isDeveloperFundPhase() const;
    bool isCPUFriendlyPhase() const;
    bool isHalvingPhase() const;
    uint64_t getRemainingCPUFriendlyCoins() const;
    uint64_t getCPUFriendlyProgress() const;
    
    // Utility
    std::string getStatus() const;
    std::string formatCoins(uint64_t units) const;

private:
    // Constants (using units: 1 DIN = 1,000,000 units)
    static constexpr uint64_t COIN = 1000000ULL;                           // 1e6 units per DIN
    static constexpr uint64_t TOTAL_SUPPLY = 99ULL * 1000000ULL * COIN;    // 99M DIN = 99T units
    static constexpr uint64_t DEVELOPER_FUND = 0ULL;   // No premine — all coins are mined
    static constexpr uint64_t CPU_FRIENDLY_TARGET = 18ULL * 1000000ULL * COIN; // 18M DIN (mining)
    static constexpr uint64_t PHASE_2_START = DEVELOPER_FUND + CPU_FRIENDLY_TARGET; // 20M DIN
    
    std::atomic<uint64_t> total_coins_mined_{0};
    std::atomic<bool> developer_fund_complete_{false};
    std::atomic<bool> cpu_friendly_complete_{false};
    
    // Helper methods
    void checkPhaseTransition();
    std::string formatLargeNumber(uint64_t value) const;
};

} // namespace dinero
