#pragma once
#include <cstdint>
#include <vector>

namespace dinero {

struct VestingTranche {
    uint32_t unlock_height;   // absolute height
    uint64_t amount_sats;     // una
};

struct VestingSchedule {
    std::vector<VestingTranche> tranches;
    uint64_t total() const;   // sum of tranche amounts
};

// Pure logic; no JSON here
uint64_t vestedAmountAt(const VestingSchedule& vs, uint32_t height);
bool isSpendAllowed(const VestingSchedule& vs, uint32_t height, uint64_t cumulative_spent);

// Inline implementations for simple functions
inline uint64_t vestedAmountAt(const VestingSchedule& vs, uint32_t h) {
    uint64_t sum = 0;
    for (const auto& t : vs.tranches) {
        if (h >= t.unlock_height) {
            sum += t.amount_sats;
        }
    }
    return sum;
}

inline bool isSpendAllowed(const VestingSchedule& vs, uint32_t h, uint64_t spent) {
    return spent <= vestedAmountAt(vs, h);
}

} // namespace dinero