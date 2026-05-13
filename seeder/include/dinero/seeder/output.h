// Emit the seeder's verdict — which peers are "healthy enough" to be
// suggested as fixed seeds — into a file compatible with
// contrib/seeds/seeds_main.txt. The maintainer can then either copy
// the output directly into seeds_main.txt for the next release or
// diff against the curated list to find candidates.

#pragma once

#include "dinero/seeder/peer_state.h"

#include <chrono>
#include <string>

namespace dinero {
namespace seeder {

struct OutputConfig {
    // A peer is "healthy" if its last_success_unix is within this
    // window of now AND attempts > 0 AND success_ratio >=
    // min_success_ratio.
    std::chrono::seconds healthy_window{24 * 60 * 60};
    double min_success_ratio = 0.6;
    size_t max_entries = 200;  // cap for the output file
};

// Write a seeds_main.txt-compatible file: one `ip:port` per line plus
// inline `# uptime XX% / N attempts` comments. Header line points back
// at the seeder so the maintainer knows the file is generated.
bool write_seeds_observed(const PeerStore& store,
                          const std::string& output_path,
                          const OutputConfig& cfg);

}  // namespace seeder
}  // namespace dinero
