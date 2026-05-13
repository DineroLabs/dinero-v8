#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace dinero {

/**
 * Thread-safe mining statistics shared by the legacy Miner and MiningManager v2.
 *
 * Keep this as the single dinero::MiningStats definition. Defining this struct
 * independently in multiple headers breaks LTO/ODR builds.
 */
struct MiningStats {
    std::atomic<bool> is_mining{false};
    std::atomic<uint32_t> active_threads{0};
    std::atomic<uint64_t> total_hashes{0};
    std::atomic<double> current_hashrate{0.0};
    std::atomic<uint64_t> blocks_found{0};
    std::atomic<uint64_t> jobs_processed{0};
    std::atomic<uint64_t> mining_start_time{0};
    std::atomic<uint64_t> last_block_time{0};

    std::string current_job_id;
    uint32_t current_height{0};
    uint32_t current_difficulty{0};
    std::string mining_phase;
};

} // namespace dinero
