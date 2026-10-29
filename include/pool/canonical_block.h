#pragma once
#include "pool/pool_manager.h"
namespace dinero { class ChainstateService; }
namespace dinero::pool {
struct CanonicalPoolShare {
    std::string worker,job,hash,uid;
    double difficulty{0};
    uint32_t height{0};
    uint64_t reward{0};
};
// Actual valid, non-stale found-block RPC path. Holds selected ownership through
// the existing DB-only share/block accounting transaction; no wallet callbacks.
PoolManager::ShareSubmitResult RecordCanonicalPoolShare(
    const std::shared_ptr<ChainstateService>&,PoolManager&,const CanonicalPoolShare&);
}
