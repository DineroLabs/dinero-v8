#pragma once

#include <algorithm>
#include <cstdint>

namespace dinero::consensus {

// An unset height never activates, including at UINT32_MAX itself.
constexpr bool SixtySecondActive(uint64_t height, uint32_t activation) {
    return activation != UINT32_MAX && height >= activation;
}

constexpr uint32_t TargetSpacingAtHeight(uint64_t height, uint32_t activation,
                                        uint32_t legacy_spacing = 120) {
    return SixtySecondActive(height, activation) ? 60 : legacy_spacing;
}

// Integrate intervals (anchor, target], so the interval ending at the first
// activated block is 60 seconds. Historical intervals retain their old length.
// This changes only the ASERT clock, preserving its anchor and arithmetic.
constexpr int64_t ExpectedBlockElapsed(int64_t anchor, int64_t target,
                                       int64_t legacy_spacing, uint32_t activation) {
    const int64_t legacy_elapsed = (target - anchor) * legacy_spacing;
    if (activation == UINT32_MAX) return legacy_elapsed;
    const auto new_intervals = [activation](int64_t height) {
        return std::max<int64_t>(0, height - static_cast<int64_t>(activation) + 1);
    };
    return legacy_elapsed - (new_intervals(target) - new_intervals(anchor)) *
                            (legacy_spacing - 60);
}

} // namespace dinero::consensus
