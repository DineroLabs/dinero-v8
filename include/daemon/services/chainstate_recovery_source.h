// Node-side adapter: ChainstateService as the wallet's R1 recovery source
// (Task 6.3). The node owns ChainstateService and this adapter; the wallet
// owns the recovery logic and the interface. The adapter ONLY forwards the
// four service calls and their StatusOr/Status results (R3: every decision,
// precondition and exception message lives wallet-side).
#pragma once

#include "wallet/runtime_recovery_source.h"
#include "wallet/runtime_recovery_limits.h"

namespace dinero {
class ChainstateService;

class ChainstateRuntimeRecoverySource final : public wallet::IRuntimeRecoverySource {
public:
    explicit ChainstateRuntimeRecoverySource(ChainstateService& chain,
        size_t material_limit_bytes = kWalletRecoveryMaterialLimitBytes)
        : chain_(chain), material_limit_bytes_(material_limit_bytes) {}

    StatusOr<std::shared_ptr<const RuntimeOutboxPage>> GetDeliveryPage(
        const RuntimeOutboxCursor& after, size_t maximum_events) const override;
    StatusOr<std::shared_ptr<const RuntimeAccountReplay>> GetAccountReplay() const override;
    StatusOr<std::shared_ptr<const RuntimeWalletCoverageProjection>> GetWalletCoverage(
        WalletManager& wallet, uint64_t expected_session, UTXOIndex& index) const override;
    Status ReconcileWalletCoverage(WalletManager& wallet, UTXOIndex& index,
                                   const RuntimeWalletCoverageProjection& coverage) const override;

private:
    ChainstateService& chain_;
    size_t material_limit_bytes_;
};

} // namespace dinero
