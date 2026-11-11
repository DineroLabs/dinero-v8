#pragma once
#include "consensus/outpoint.h"
#include "wallet/runtime_account_replay.h"
#include <vector>

namespace dinero {
// Transparent per-block effects of the verified post-base tail, extracted for
// the snapshot-recovery rescan writer (v6-recovery stage C). DATA only:
// ordering and linkage are validated against the replay, but ownership,
// maturity and balance remain the consuming writer's decisions. Confidential
// outputs are carried with their flag, never resolved here; the writer refuses
// owned confidential outputs exactly as it does for raw bodies.
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
// Walks the replay's verified activation prefix (when present) and its outbox
// events, folding connects/disconnects into the final selected chain. Throws
// std::runtime_error on any structural violation (fail closed): a base other
// than the activation parent, broken linkage, a disconnect below the first
// tail block, a coinbase that is not the historical first transaction, or
// capacity overruns. Capacity counters are not rolled back on disconnect;
// they bound work, not the result.
RecoveryTailEffects ReadRecoveryTailEffects(const RuntimeAccountReplay& replay);
}  // namespace dinero
