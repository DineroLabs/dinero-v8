#pragma once
#include "daemon/mempool_transaction.h"
#include "daemon/interfaces/ingress_types.h"
#include <span>
#include <map>

namespace dinero {
// Borrowed only during a selected-chain/pool-owned validation call. The wire
// bytes are untrusted input, even when retrieved from an existing pool cache.
struct MempoolProofView {
    MempoolTransaction body;
    std::span<const uint8_t> proof;
};
struct MempoolOrchardValidation {
    TxAcceptResult result = TxAcceptResult::Rejected(TxRejectCode::UNAVAILABLE,
        "Selected Orchard validator unavailable");
    uint32_t parent_height = 0;
    uint64_t fee = 0;
    std::vector<uint8_t> proof_root; // Nonempty only after exact selected proof validation.
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
    virtual MempoolSelectionValidation ValidateBlockSelectionWithProofs(
        std::span<const MempoolProofView> entries, uint32_t height) {
        std::vector<MempoolTransaction> bodies;
        for (const auto& entry : entries) {
            if (!entry.proof.empty()) return {};
            bodies.push_back(entry.body);
        }
        return ValidateBlockSelection(bodies, height);
    }
    virtual MempoolOrchardValidation ValidateOrchardWithProofs(
        const MempoolProofView& incoming, std::span<const MempoolProofView> pending) {
        if (!incoming.proof.empty()) return {};
        std::vector<MempoolTransaction> bodies;
        for (const auto& entry : pending) {
            if (!entry.proof.empty()) return {};
            bodies.push_back(entry.body);
        }
        return ValidateOrchard(incoming.body, bodies);
    }

    virtual MempoolSelectionValidation ValidateBlockSelection(
        std::span<const MempoolTransaction>, uint32_t) { return {}; }
    virtual MempoolOrchardValidation ValidateOrchard(
        const MempoolTransaction&, std::span<const MempoolTransaction>) { return {}; }
};
} // namespace dinero
