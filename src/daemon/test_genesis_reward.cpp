#include <iostream>
#include <iomanip>
#include "consensus/dinero_algorithm.h"

int main() {
    std::cout << "🚀 Dinero (DIN) Algorithm Test" << std::endl;
    std::cout << "=================================" << std::endl;
    
    // Test genesis reward calculation using the new algorithm
    dinero::Blockchain blockchain("./test_genesis_data");
    
    std::cout << "\n1️⃣ Testing genesis reward calculation..." << std::endl;
    uint64_t genesisReward = dinero::DineroAlgorithm::calculateBlockReward(0, 0);
    std::cout << "   - Genesis reward: " << genesisReward << " units" << std::endl;
    std::cout << "   - Genesis reward: " << dinero::DineroAlgorithm::formatCoins(genesisReward) << std::endl;
    
    // Test easy phase rewards (99 DIN per block until 20M coins)
    std::cout << "\n2️⃣ Testing easy phase rewards..." << std::endl;
    for (uint32_t h = 1; h <= 5; h++) {
        uint64_t reward = dinero::DineroAlgorithm::calculateBlockReward(h, 0);
        std::cout << "   - Height " << h << ": " << reward << " units (" << dinero::DineroAlgorithm::formatCoins(reward) << ")" << std::endl;
    }
    
    // Test halving phase boundaries
    std::cout << "\n3️⃣ Testing halving phase boundaries..." << std::endl;
    const uint32_t EASY_BLOCKS = 181818; // 18M coins / 99 DIN per block
    const uint32_t BLOCKS_PER_HALVING = 210000;
    
    uint32_t halving_boundaries[] = {
        EASY_BLOCKS + 1,                    // First block after easy phase
        EASY_BLOCKS + BLOCKS_PER_HALVING,   // First halving
        EASY_BLOCKS + 2 * BLOCKS_PER_HALVING // Second halving
    };
    
    for (uint32_t height : halving_boundaries) {
        uint64_t reward = dinero::DineroAlgorithm::calculateBlockReward(height, 0);
        std::cout << "   - Height " << height << ": " << reward << " units (" << dinero::DineroAlgorithm::formatCoins(reward) << ")" << std::endl;
    }
    
    // Test blocks just before and after halving boundaries
    std::cout << "\n4️⃣ Testing blocks around halving boundaries..." << std::endl;
    
    // Easy phase to halving transition
    std::cout << "   Easy phase → Halving transition:" << std::endl;
    std::cout << "   - Height " << EASY_BLOCKS << ": " << dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS, 0) << " units (" << dinero::DineroAlgorithm::formatCoins(dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS, 0)) << ")" << std::endl;
    std::cout << "   - Height " << (EASY_BLOCKS + 1) << ": " << dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + 1, 0) << " units (" << dinero::DineroAlgorithm::formatCoins(dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + 1, 0)) << ")" << std::endl;
    
    // First halving
    std::cout << "   First halving:" << std::endl;
    std::cout << "   - Height " << (EASY_BLOCKS + BLOCKS_PER_HALVING - 1) << ": " << dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + BLOCKS_PER_HALVING - 1, 0) << " units (" << dinero::DineroAlgorithm::formatCoins(dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + BLOCKS_PER_HALVING - 1, 0)) << ")" << std::endl;
    std::cout << "   - Height " << (EASY_BLOCKS + BLOCKS_PER_HALVING) << ": " << dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + BLOCKS_PER_HALVING, 0) << " units (" << dinero::DineroAlgorithm::formatCoins(dinero::DineroAlgorithm::calculateBlockReward(EASY_BLOCKS + BLOCKS_PER_HALVING, 0)) << ")" << std::endl;
    
    std::cout << "\n✅ Dinero Algorithm Test completed!" << std::endl;
    std::cout << "   - Genesis block: 100K DIN (100B units)" << std::endl;
    std::cout << "   - Easy phase (181,818 blocks): 99 DIN per block" << std::endl;
    std::cout << "   - Halving phase: 187.857142 DIN starting, halving every 210K blocks" << std::endl;
    std::cout << "   - Total supply: 99M DIN" << std::endl;
    std::cout << "   - Ticker: DIN" << std::endl;
    
    return 0;
} 