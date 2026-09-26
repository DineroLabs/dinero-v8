#pragma once
#include "primitives/uint256.h"
#include <cstdint>

namespace dinero::storage {
// Persistence receipt, NOT proof of historical balance or SHR1 correctness.
// The connector must authenticate these fields at the selected boundary and
// enforce frozen contents against them. Never seed this from a wallet balance.
struct LegacyRetirementRecord {
    uint8_t network_code = 0xff;
    uint256 genesis;
    uint32_t branch_id = 0;
    uint32_t activation_height = 0;
    uint32_t legacy_epoch_height = 0;
    uint256 boundary_parent;
    uint256 legacy_state_root; // Historical composite SHR1, not just tree root.
    uint64_t retired_value = 0;
    uint256 tree_root;
    uint64_t tree_size = 0;
    uint64_t nullifier_count = 0;
    bool operator==(const LegacyRetirementRecord&) const = default;
};
struct LegacyRetirementState {
    LegacyRetirementRecord record;
    uint32_t height = 0;
    uint256 block_hash;
    uint256 parent_hash;
    bool operator==(const LegacyRetirementState&) const = default;
};
} // namespace dinero::storage
