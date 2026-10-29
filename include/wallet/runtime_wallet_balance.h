#pragma once
#include "wallet/wallet_manager.h"
#include "daemon/runtime_outbox_cursor.h"

namespace dinero {
// Immutable result of one owned read, not a reusable source/coin capability or
// whole-wallet readiness certificate. No caller or deserializer can mint it.
class RuntimeWalletBalanceSnapshot final {
public:
    const WalletManager::BalanceSummary& Summary() const { return summary_; }
    const RuntimeOutboxCursor& Cursor() const { return cursor_; }
    const uint256& TipHash() const { return tip_; }
    uint32_t TipHeight() const { return height_; }
private:
    friend class ChainstateService;
    RuntimeWalletBalanceSnapshot(WalletManager::BalanceSummary summary,RuntimeOutboxCursor cursor,
        uint256 tip,uint32_t height):summary_(std::move(summary)),cursor_(cursor),tip_(tip),height_(height){}
    WalletManager::BalanceSummary summary_;
    RuntimeOutboxCursor cursor_;
    uint256 tip_;
    uint32_t height_;
};
} // namespace dinero
