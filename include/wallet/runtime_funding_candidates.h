#pragma once
#include "consensus/outpoint.h"
#include <set>
#include <vector>

namespace dinero {
// PUBLIC candidate DATA only, never a coin view, baseline, capability or receipt.
// The selected service rebuilds this from complete authenticated post-base bodies;
// every eventual spend still requires its own CURRENT source-bound input proof.
struct RuntimeFundingCoin {
    OutPoint point;
    uint64_t value=0;
    std::vector<uint8_t> script;
    uint32_t height=0;
    bool coinbase=false;
};
struct RuntimeFundingCandidates {
    uint32_t height=0;
    uint256 block;
    std::vector<RuntimeFundingCoin> coins;
    // Actual post-base spends and authenticated current/retained/pending owners.
    // Read-only exclusion; never delete/rebase an ordinary or frozen wallet row.
    std::set<OutPoint> unavailable;
};
}
