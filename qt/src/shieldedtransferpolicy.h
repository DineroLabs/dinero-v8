#pragma once

#include <QString>

namespace ShieldedTransferPolicy {
inline bool maySubmit(const QString& stage, bool operationRunning) {
    return !operationRunning && (stage.isEmpty() || stage == "rejected");
}

inline bool uncertainAfterRestart(const QString& stage) {
    return stage == "submitting";
}
}  // namespace ShieldedTransferPolicy
