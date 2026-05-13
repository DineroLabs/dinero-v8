// Minimal AssumeUTXORegistry stub for test compilation
#pragma once

#include "../../../../include/primitives/uint256.h"
#include <optional>
#include <cstdint>

namespace dinero {
namespace consensus {

struct SnapshotInfo {
    uint256 block_hash;
    uint32_t block_height;
    uint64_t utxo_count;
};

class AssumeUTXORegistry {
public:
    static std::optional<SnapshotInfo> GetSnapshot(uint32_t height);
};

} // namespace consensus
} // namespace dinero
