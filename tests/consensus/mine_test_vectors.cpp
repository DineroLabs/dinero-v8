// One-time mining tool to generate valid test vectors
// Run once, copy output nonces into test_header_sync_restart.cpp

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include "primitives/block.h"
#include "consensus/pow.h"

using namespace dinero;

std::atomic<bool> found{false};
std::atomic<uint32_t> winning_nonce{0};

void mine_thread(BlockHeader h, int thread_id, int num_threads) {
    for (uint64_t n = thread_id; n < 0xFFFFFFFFULL && !found.load(); n += num_threads) {
        h.nonce = static_cast<uint32_t>(n);
        if (consensus::CheckProofOfWork(h, false)) {
            found.store(true);
            winning_nonce.store(h.nonce);
            return;
        }
    }
}

int main() {
    const int NUM_THREADS = 16;
    
    std::cout << "// Pre-computed test vectors for test_header_sync_restart.cpp\n";
    std::cout << "// Mining at difficulty 0x1d00ffff with " << NUM_THREADS << " threads\n\n";
    
    std::string prev_hash = std::string(64, '0');  // Genesis
    
    for (int height = 1; height <= 4; height++) {
        BlockHeader header;
        header.version = 1;
        header.prev_block_hash = uint256::FromHexUnsafe(prev_hash);
        header.merkle_root = uint256();
        header.timestamp = 1000000 + height;
        header.difficulty = 0x1d00ffff;
        header.nonce = 0;
        header.utreexo_root = uint256();
        std::memset(header.reserved, 0, 12);
        
        found.store(false);
        winning_nonce.store(0);
        
        std::cerr << "Mining header " << height << "..." << std::flush;
        auto start = std::chrono::high_resolution_clock::now();
        
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; t++) {
            threads.emplace_back(mine_thread, header, t, NUM_THREADS);
        }
        for (auto& t : threads) t.join();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
        
        header.nonce = winning_nonce.load();
        std::string hash = header.GetHash().GetHex();
        
        std::cerr << " done (" << secs << "s)\n";
        
        std::cout << "// Header " << height << ": prev=" << prev_hash.substr(0,16) << "...\n";
        std::cout << "static constexpr uint32_t HEADER_" << height << "_NONCE = " << header.nonce << ";\n";
        std::cout << "// hash: " << hash.substr(0,16) << "...\n\n";
        
        prev_hash = hash;
    }
    
    return 0;
}
