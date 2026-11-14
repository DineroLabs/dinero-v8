#pragma once
#include "consensus/outpoint.h"
#include <vector>

namespace dinero {
// Transparent per-block effects of the verified post-base tail (v6-recovery
// stage C). DATA only: ordering and linkage are validated by the producer
// (ReadRecoveryTailEffects) and by SelectedWalletHistory::Builder, but
// ownership, maturity and balance remain the consuming writer's decisions.
// Kept free of Orchard/runtime headers so the wallet core can consume them in
// every build configuration.
struct RecoveryTailOutput {
    uint64_t value_una = 0;
    std::vector<uint8_t> script_pub_key;
    bool is_confidential = false;
};
struct RecoveryTailTransaction {
    TxId txid;
    // Empty for the coinbase. Unfiltered: non-owned prevouts are no-ops to the
    // writer, and filtering would require the owned set here.
    std::vector<OutPoint> spends;
    // Positional: index == vout. Zero-value and confidential outputs are kept
    // so OutPoint identities match the real block exactly.
    std::vector<RecoveryTailOutput> outputs;
};
struct RecoveryTailBlock {
    uint256 hash;
    uint256 parent;
    uint32_t height = 0;
    uint32_t timestamp = 0;
    std::vector<RecoveryTailTransaction> transactions;  // index 0 is the coinbase
};
struct RecoveryTailEffects {
    uint32_t base_height = 0;  // activation parent: the base the V5 coins stand on
    uint256 base_hash;
    std::vector<RecoveryTailBlock> blocks;  // the selected chain base+1..tip
};
}  // namespace dinero
