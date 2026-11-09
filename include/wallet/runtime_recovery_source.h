// Wallet-side recovery source interface (Task 6.3, requirement R1).
//
// RuntimeWalletRecovery's public entry points observe the node ONLY through
// this interface: one operation per call the node adapter makes, returning
// the neutral StatusOr/Status from common/status.h exactly as the
// ChainstateService calls do. An implementation never throws for an
// unavailable source — the wallet decides what unavailability means (R3).
//
// Node-free check (plan Task 6.3 R1/Step 2, verified 2026-11-07): the types
// come from wallet headers and daemon/runtime_block_outbox.h, whose include
// closure (runtime_outbox_cursor.h, runtime_block_notifications.h,
// consensus/orchard_state_transition.h -> storage/orchard_state.h) is
// uint256/typed-value material only — no RocksDB, no services, no src/daemon.
#pragma once

#include "common/status.h"
#include "daemon/runtime_block_outbox.h"       // RuntimeOutboxPage, RuntimeOutboxCursor
#include "wallet/runtime_account_replay.h"      // RuntimeAccountReplay
#include "wallet/runtime_origin_projection.h"   // RuntimeWalletCoverageProjection

#include <cstdint>
#include <memory>

namespace dinero {
class WalletManager;
class UTXOIndex;

namespace wallet {

class IRuntimeRecoverySource {
public:
    virtual ~IRuntimeRecoverySource() = default;

    // The bounded, checked canonical delivery page after `after`.
    virtual StatusOr<std::shared_ptr<const RuntimeOutboxPage>> GetDeliveryPage(
        const RuntimeOutboxCursor& after, size_t maximum_events) const = 0;

    // The captured account replay view.
    virtual StatusOr<std::shared_ptr<const RuntimeAccountReplay>> GetAccountReplay() const = 0;

    // The checked wallet coverage projection for this wallet/index/session.
    virtual StatusOr<std::shared_ptr<const RuntimeWalletCoverageProjection>> GetWalletCoverage(
        WalletManager& wallet, uint64_t expected_session, UTXOIndex& index) const = 0;

    // Reconcile the projection into the wallet/index owners.
    virtual Status ReconcileWalletCoverage(
        WalletManager& wallet, UTXOIndex& index,
        const RuntimeWalletCoverageProjection& coverage) const = 0;
};

} // namespace wallet
} // namespace dinero
