// wallet_spendability.h
//
// #353: on a utreexo / AssumeUTXO-bootstrapped node, the wallet can hold coins that
// are NOT in the node's active spendable set (accumulator-anchored, no inclusion proof
// on the plain send path). Coin-selection was offering those coins, so the auto-picker
// grabbed one and the node rejected the whole tx ("input not in active UTXO set").
//
// This partitions the wallet's UTXOs by whether the node can actually spend each one,
// using an injected consensus check (ChainStateView::hasCoin in production, a fake in
// tests). Anchored coins are KEPT (still visible in the total balance — #338/#340 made
// them visible on purpose) but excluded from the spendable/selectable set. Pure and
// dependency-injected so it is unit-testable without a live anchored node.
#pragma once

#include "wallet/canonical_wallet_utxo.h"
#include "primitives/uint256.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace dinero {
namespace wallet {

/// Injected consensus check: is this outpoint in the node's active, spendable UTXO set?
/// Backed by ChainStateView::hasCoin(outpoint) in production.
using SpendableInActiveSetFn = std::function<bool(const uint256& txid, uint32_t vout)>;

struct SpendabilityPartition {
    std::vector<CanonicalWalletUTXO> spendable;  // in active set — safe to offer to coin-selection
    std::vector<CanonicalWalletUTXO> anchored;   // not in active set — visible but un-spendable
    uint64_t spendable_una = 0;
    uint64_t anchored_una = 0;
};

/// Split `utxos` into spendable vs anchored using `is_spendable`. A null/empty check
/// treats everything as spendable (preserves legacy behavior on nodes that don't
/// distinguish an active set).
SpendabilityPartition PartitionBySpendability(
    const std::vector<CanonicalWalletUTXO>& utxos,
    const SpendableInActiveSetFn& is_spendable);

}  // namespace wallet
}  // namespace dinero
