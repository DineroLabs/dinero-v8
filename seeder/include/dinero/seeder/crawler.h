// Main crawl loop: pull from a bootstrap list, probe peers in rotation,
// feed learned addresses back into the queue, persist state, write
// observed-healthy list on a periodic cadence. Single-threaded blocking
// I/O for v8.0.0 — concurrency is a future optimization.

#pragma once

#include "dinero/seeder/connection.h"
#include "dinero/seeder/output.h"
#include "dinero/seeder/peer_state.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace dinero {
namespace seeder {

struct CrawlerConfig {
    std::vector<std::string> bootstrap;       // "host:port" strings
    std::string state_path;                    // peer-state JSON file
    std::string output_path;                   // seeds_observed.txt
    uint16_t default_port = 20999;             // mainnet listen port
    size_t batch_size = 8;                     // peers probed per cycle
    std::chrono::seconds cycle_pause{30};      // between cycle batches
    std::chrono::seconds output_interval{300}; // re-write output every 5 min
    std::chrono::seconds state_save_interval{120};  // persist state every 2 min
    std::chrono::seconds total_duration{0};    // 0 = run until interrupted
    ProbeConfig probe;
    OutputConfig output;
};

// Run the crawler until the duration elapses or stop_flag is set.
// Returns the number of probe attempts completed (any outcome).
size_t run_crawler(const CrawlerConfig& cfg, std::atomic<bool>& stop_flag);

}  // namespace seeder
}  // namespace dinero
