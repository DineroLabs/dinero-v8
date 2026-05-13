#include "primitives/vesting.h"
#include <algorithm>

namespace dinero {

// VestingSchedule implementation
uint64_t VestingSchedule::total() const {
    uint64_t sum = 0;
    for (const auto& tranche : tranches) {
        sum += tranche.amount_sats;
    }
    return sum;
}

} // namespace dinero