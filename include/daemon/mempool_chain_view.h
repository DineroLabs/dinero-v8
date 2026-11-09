#pragma once

#include "common/status.h"
#include "primitives/uint256.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

// Values copied across the node/library boundary; no database types escape.
struct MempoolChainTip {
    int height = 0;
    uint256 hash;
};
struct MempoolChainCoin {
    uint64_t amount = 0;
    std::string script_pubkey;
    int height = 0;
    bool coinbase = false;
    bool is_confidential = false;
    std::vector<uint8_t> commitment;
};

// Borrowed for the pool's lifetime. Implementations must preserve the caller's
// existing chain-guard/pool-lock order. This interface supplies no snapshot,
// caching, retry or additional locking contract.
class IMempoolChainView {
public:
    virtual ~IMempoolChainView() = default;
    virtual StatusOr<MempoolChainTip> GetTip() const = 0;
    virtual StatusOr<MempoolChainCoin> GetCoinWithConfidentialFallback(
        const uint256& txid, uint32_t vout) const = 0;
    virtual bool HaveTransaction(const uint256& txid) const = 0;
    virtual bool HaveConfidentialCommitment(const std::vector<uint8_t>& commitment) const = 0;
};
} // namespace dinero
