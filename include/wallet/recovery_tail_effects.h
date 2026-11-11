#pragma once
#include "wallet/recovery_tail_types.h"
#include "wallet/runtime_account_replay.h"

namespace dinero {
// Walks the replay's verified activation prefix (when present) and its outbox
// events, folding connects/disconnects into the final selected chain. Throws
// std::runtime_error on any structural violation (fail closed): a base other
// than the activation parent, broken linkage, a disconnect below the first
// tail block, a coinbase that is not the historical first transaction, or
// capacity overruns. Capacity counters are not rolled back on disconnect;
// they bound work, not the result. Confidential outputs are carried with
// their flag, never resolved here; the writer refuses owned confidential
// outputs exactly as it does for raw bodies.
RecoveryTailEffects ReadRecoveryTailEffects(const RuntimeAccountReplay& replay);
}  // namespace dinero
