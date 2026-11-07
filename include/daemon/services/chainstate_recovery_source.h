// Node-side adapter: ChainstateService as the wallet's R1 recovery source
// (Task 6.3). The node owns ChainstateService and this adapter; the wallet
// owns the recovery logic and the interface. The adapter ONLY forwards the
// four service calls and their StatusOr/Status results (R3: every decision,
// precondition and exception message lives wallet-side).
#pragma once

#include "wallet/runtime_recovery_source.h"

namespace dinero {
class ChainstateService;

class ChainstateRuntimeRecoverySource final : public wallet::IRuntimeRecoverySource {
public:
    explicit ChainstateRuntimeRecoverySource(ChainstateService& chain) : chain_(chain) {}

    StatusOr<std::shared_ptr<const RuntimeOutboxPage>> GetDeliveryPage(
        const RuntimeOutboxCursor& after, size_t maximum_events) const override;
    StatusOr<std::shared_ptr<const RuntimeAccountReplay>> GetAccountReplay() const override;
    StatusOr<std::shared_ptr<const RuntimeWalletCoverageProjection>> GetWalletCoverage(
        WalletManager& wallet, uint64_t expected_session, UTXOIndex& index) const override;
    Status ReconcileWalletCoverage(WalletManager& wallet, UTXOIndex& index,
                                   const RuntimeWalletCoverageProjection& coverage) const override;

private:
    ChainstateService& chain_;
};

} // namespace dinero
