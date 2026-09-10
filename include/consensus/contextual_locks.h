#pragma once

#include "primitives/transaction.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dinero::consensus {

// Input heights are confirmed creation heights. A mempool parent uses the
// candidate block height. Unknown metadata is not height zero.
using LockMtpLookup = std::function<std::optional<uint64_t>(uint32_t)>;
inline bool CheckContextualLocks(const Transaction& tx, uint32_t height,
    uint32_t activation, const std::vector<std::optional<uint32_t>>& input_heights,
    const LockMtpLookup& branch_mtp, std::string& error, bool defer_unknown_relative = false) {
    if (activation == UINT32_MAX || height < activation) return true;
    constexpr uint32_t threshold = 500000000U;
    constexpr uint32_t disabled = 1U << 31;
    constexpr uint32_t time_based = 1U << 22;
    const bool all_final = std::all_of(tx.vin.begin(), tx.vin.end(),
        [](const auto& input) { return input.sequence == UINT32_MAX; });
    if (tx.lockTime != 0 && !all_final) {
        uint64_t cutoff = height;
        if (tx.lockTime >= threshold) {
            auto mtp = height && branch_mtp ? branch_mtp(height - 1) : std::nullopt;
            if (!mtp) { error = "contextual-lock-missing-parent-mtp"; return false; }
            cutoff = *mtp;
        }
        if (tx.lockTime >= cutoff) { error = "non-final-absolute-lock"; return false; }
    }
    if (tx.version < 2) return true;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const uint32_t sequence = tx.vin[i].sequence;
        if (sequence & disabled) continue;
        // Only stateless callers may explicitly defer legacy, unauthenticated
        // creation metadata. Known v2-leaf ages are always checked.
        if (defer_unknown_relative && i < input_heights.size() && !input_heights[i]) continue;
        if (i >= input_heights.size() || !input_heights[i] || *input_heights[i] > height) {
            error = "contextual-lock-missing-input-height"; return false;
        }
        const uint64_t delay = sequence & 0xffffU;
        if (sequence & time_based) {
            const uint32_t creation_parent = *input_heights[i] == 0 ? 0 : *input_heights[i] - 1;
            auto origin = branch_mtp ? branch_mtp(creation_parent) : std::nullopt;
            auto current = height && branch_mtp ? branch_mtp(height - 1) : std::nullopt;
            if (!origin || !current) { error = "contextual-lock-missing-input-mtp"; return false; }
            if (*origin > UINT64_MAX - delay * 512 || *current < *origin + delay * 512) {
                error = "non-final-relative-time-lock"; return false;
            }
        } else if (static_cast<uint64_t>(height) < static_cast<uint64_t>(*input_heights[i]) + delay) {
            error = "non-final-relative-height-lock"; return false;
        }
    }
    return true;
}
} // namespace dinero::consensus
