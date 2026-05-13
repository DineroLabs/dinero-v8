// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — reorg detection.
// Daemon-side port of `Core/Vault/ReorgWatcher.swift`.
//
// On every chain tip change, the watcher iterates every credited-or-
// settled deposit and asks: is this deposit's tx still in the active
// chain at its original height with the original block hash?
//
// On the daemon side this hooks into validation.cpp's DisconnectTip
// directly — a real reorg signal, not a polling loop. The Swift
// implementation had to invent `txIncludedAt` because the iOS client
// has no direct chain-disconnect event; daemon-side we get it for free.

#pragma once

#include "vault/deposit_flow.h"
#include "vault/vault_types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace dinero::vault {

/// Per-tip chain query result for one deposit.
enum class ChainInclusion : uint8_t {
    /// Tx still in active chain at original height + block hash.
    STILL_INCLUDED,
    /// Block at the deposit's height changed hash but the tx is in
    /// the new block (just re-mined into a sibling).
    RE_MINED_SAME_TXID,
    /// Tx is gone from the active chain. Reorg evicted it.
    ORPHANED,
    /// Chain query couldn't answer (block not yet fetched / pruned).
    UNKNOWN,
};

class ReorgError : public std::runtime_error {
   public:
    enum class Kind : uint8_t {
        DEPOSIT_FLOW,
        UNRECORDED_OBSERVATION,
    };
    ReorgError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

/// Single-threaded chain watcher. The vault service drives it from
/// the same code path that owns the DepositFlowMachine.
///
/// `block_hash_at_height` and `tx_included_at` are dependency-injected
/// closures so this class has zero coupling to chainstate types. Real
/// daemon wiring passes closures pointing at `chainstate->blockAtHeight`
/// and a tx-inclusion check against the live UTXO set.
class ReorgWatcher {
   public:
    using BlockHashAtHeightFn = std::function<std::array<uint8_t, 32>(uint64_t)>;
    using TxIncludedAtFn =
        std::function<bool(const OutpointId&, uint64_t, const std::array<uint8_t, 32>&)>;

    ReorgWatcher(DepositFlowMachine* machine, BlockHashAtHeightFn block_hash_at_height,
                 TxIncludedAtFn tx_included_at)
        : machine_{machine},
          block_hash_at_height_{std::move(block_hash_at_height)},
          tx_included_at_{std::move(tx_included_at)} {}

    /// Record the block hash for a freshly-observed deposit. Caller
    /// invokes this at the moment a UTXO is added; the deposit's
    /// enclosing block hash comes from chainstate. Idempotent: a
    /// duplicate call leaves the recorded hash alone.
    void recordObservation(const OutpointId& outpoint, const std::array<uint8_t, 32>& block_hash);

    /// On every new tip, re-check every tracked deposit at risk.
    /// Returns the count of revert() calls emitted.
    int tipChanged(uint64_t tip_height);

    [[nodiscard]] const std::unordered_map<OutpointId, std::array<uint8_t, 32>>& depositBlockHashes() const noexcept {
        return deposit_block_hashes_;
    }

   private:
    ChainInclusion check(const TrackedDeposit& dep);
    UnaAmount unrecoverableLoss(const TrackedDeposit& dep);

    DepositFlowMachine* machine_{nullptr};
    BlockHashAtHeightFn block_hash_at_height_;
    TxIncludedAtFn tx_included_at_;
    std::unordered_map<OutpointId, std::array<uint8_t, 32>> deposit_block_hashes_;
};

}  // namespace dinero::vault
