#pragma once
#include "daemon/mempool_transaction.h"
#include "daemon/interfaces/ingress_types.h"
#include <span>

namespace dinero {
struct MempoolOrchardValidation {
    TxAcceptResult result = TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,
        "Selected Orchard validator unavailable");
    uint32_t parent_height = 0;
    uint64_t fee = 0;
};
// Held before the pool lock through validation and publication. Results are
// ephemeral checks under this owner, never reusable chain-state certificates.
class MempoolChainstateReadGuard {
public:
    virtual ~MempoolChainstateReadGuard() = default;
    virtual MempoolOrchardValidation ValidateOrchard(
        const MempoolTransaction&, std::span<const MempoolTransaction>) { return {}; }
};
} // namespace dinero
