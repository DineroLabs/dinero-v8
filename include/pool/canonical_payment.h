#pragma once
#include "pool/payment_attempt.h"
namespace dinero { class ChainstateService; }
namespace dinero::pool {
class WalletPoolDispatcher;
// Private production owner: wallet authentication precedes this call. This
// owner holds the selected chain through the checked pool accounting commit.
class PoolPaymentCanonicalOwner {
    friend class WalletPoolDispatcher;
    friend struct PoolPaymentSettlementTestAccess;
    friend struct PoolAttemptSourceTestAccess;
    static PoolPaymentAttempt Begin(const std::shared_ptr<ChainstateService>&,PoolDB&,
                                    const PoolPaymentWalletBinding&,const std::vector<uint64_t>&);
    static bool Reconcile(const std::shared_ptr<ChainstateService>&,PoolDB&,const PoolPaymentAttempt&);
};
}
