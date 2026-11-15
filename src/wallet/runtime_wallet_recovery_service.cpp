// Node-side R1 adapter implementation (Task 6.3). This file is compiled into
// the node-side dinero_orchard_recovery_adapter target, NOT into the wallet
// delivery libraries: it is the only recovery file allowed to name
// ChainstateService. It forwards the four service calls verbatim; the lease
// precondition, the coverage decision and the error-message mapping moved to
// src/wallet/runtime_wallet_recovery.cpp (R3).

#include "daemon/services/chainstate_recovery_source.h"
#include "daemon/services/chainstate_service.h"

namespace dinero {

StatusOr<std::shared_ptr<const RuntimeOutboxPage>>
ChainstateRuntimeRecoverySource::GetDeliveryPage(const RuntimeOutboxCursor& after,
                                                 size_t maximum_events) const {
    return chain_.getRuntimeDeliveryPage(after, maximum_events);
}

StatusOr<std::shared_ptr<const RuntimeAccountReplay>>
ChainstateRuntimeRecoverySource::GetAccountReplay() const {
    return chain_.getRuntimeAccountReplay();
}

StatusOr<std::shared_ptr<const RuntimeWalletCoverageProjection>>
ChainstateRuntimeRecoverySource::GetWalletCoverage(WalletManager& wallet,
                                                   uint64_t expected_session,
                                                   UTXOIndex& index) const {
    return chain_.getRuntimeWalletCoverage(wallet, expected_session, index, material_limit_bytes_);
}

Status ChainstateRuntimeRecoverySource::ReconcileWalletCoverage(
        WalletManager& wallet, UTXOIndex& index,
        const RuntimeWalletCoverageProjection& coverage) const {
    return chain_.reconcileRuntimeWalletCoverage(wallet, index, coverage);
}

} // namespace dinero
