#pragma once
#include "daemon/mempool_transaction.h"
#include "daemon/interfaces/ingress_types.h"
#include <span>
#include <map>

namespace dinero {
struct MempoolOrchardValidation {
    TxAcceptResult result = TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,
        "Selected Orchard validator unavailable");
    uint32_t parent_height = 0;
    uint64_t fee = 0;
};
struct MempoolSelectionValidation {
    TxAcceptResult result = TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,
        "Selected mixed-family validator unavailable");
    uint256 parent_hash;
    uint32_t parent_height = 0;
    std::map<uint256, uint64_t> fees;
};
// Held before the pool lock through validation and publication. Results are
// ephemeral checks under this owner, never reusable chain-state certificates.
class MempoolChainstateReadGuard {
public:
    virtual ~MempoolChainstateReadGuard() = default;
    virtual MempoolSelectionValidation ValidateBlockSelection(
        std::span<const MempoolTransaction>, uint32_t) { return {}; }
    virtual MempoolOrchardValidation ValidateOrchard(
        const MempoolTransaction&, std::span<const MempoolTransaction>) { return {}; }
};
} // namespace dinero
