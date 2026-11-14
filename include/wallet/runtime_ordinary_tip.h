#pragma once
#include "primitives/uint256.h"
#include "wallet/wallet_manager.h"
#include <cstdint>
#include <optional>
#include <utility>

namespace dinero {
class WalletManager;
class UTXOIndex;

// Narrow read of the authenticated ordinary-delivery receipt for RPC source
// freshness checks. The implementation stays in the optional Orchard wallet
// delivery library; callers must not infer a whole-wallet certificate from it.
std::optional<std::pair<uint256, uint32_t>> ReadRuntimeOrdinaryWalletTip(
    WalletManager& wallet, uint64_t expected_session);

// Full-archive v3 only. A selected-tip balance must also have this wallet's
// exact index receipt; a foreign, invalidated or stale receipt cannot stand in.
bool RuntimeV3IndexWalletTipCurrent(UTXOIndex& metadata_index,
    WalletManager::DatabaseLease& current_lease,
    const uint256& selected_hash, uint32_t selected_height);
bool IsRuntimeV3MetadataIndex(const UTXOIndex& index);
}
