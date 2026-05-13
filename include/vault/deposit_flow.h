// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — deposit-flow state machine.
// Daemon-side port of `Core/Vault/DepositFlowMachine.swift`.
//
// Lifecycle:
//   detected → observed (K_OBSERVE) → credited (K_CREDIT) → settled (K_SETTLE)
//                                                         ↘ reverted
//
// On the daemon side this hooks straight into validation.cpp's
// ConnectBlock event (each block-connect is a tip change). The
// machine itself stays pure; the integration glue lives in
// `vault_service.cpp`.

#pragma once

#include "vault/ledger.h"
#include "vault/vault_types.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dinero::vault {

/// Per-deployment K-confirmation policy. See design doc §4.
struct ConfirmationPolicy {
    uint64_t k_observe{1};
    uint64_t k_credit{2};
    uint64_t k_settle{6};
    /// Per-deposit-size matrix. If a deposit's amount exceeds
    /// `threshold`, its `k_credit` is overridden upward. Sorted
    /// ascending by `threshold`; the LAST matching row wins.
    std::vector<std::pair<UnaAmount, uint64_t>> size_matrix;

    [[nodiscard]] uint64_t effectiveKCredit(UnaAmount amount) const {
        uint64_t k = k_credit;
        for (const auto& [threshold, override_k] : size_matrix) {
            if (amount >= threshold) {
                k = std::max(k, override_k);
            }
        }
        return k;
    }

    static ConfirmationPolicy defaults() { return ConfirmationPolicy{}; }
};

enum class DepositStage : uint8_t {
    DETECTED,
    OBSERVED,
    CREDITED,
    SETTLED,
    REVERTED,
};

struct TrackedDeposit {
    OutpointId outpoint;
    AccountId account;
    UnaAmount amount{0};
    uint64_t deposit_height{0};
    DepositStage stage{DepositStage::DETECTED};

    bool operator==(const TrackedDeposit&) const = default;
};

class DepositFlowError : public std::runtime_error {
   public:
    enum class Kind : uint8_t {
        LEDGER,
        UNKNOWN_DEPOSIT,
    };
    DepositFlowError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

/// Daemon-side state machine. Single-writer; the surrounding service
/// serializes calls.
class DepositFlowMachine {
   public:
    DepositFlowMachine(Ledger* ledger, ConfirmationPolicy policy = ConfirmationPolicy::defaults(),
                       bool shadow_mode = false)
        : ledger_{ledger}, policy_{std::move(policy)}, shadow_mode_{shadow_mode} {}

    void setPolicy(ConfirmationPolicy policy) { policy_ = std::move(policy); }
    void setShadowMode(bool on) { shadow_mode_ = on; }
    [[nodiscard]] bool shadowMode() const noexcept { return shadow_mode_; }
    [[nodiscard]] const ConfirmationPolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] Ledger* ledger() const noexcept { return ledger_; }
    [[nodiscard]] const std::unordered_map<OutpointId, TrackedDeposit>& tracked() const noexcept {
        return tracked_;
    }

    /// Register a newly-detected deposit. Idempotent on outpoint.
    void observe(const OutpointId& outpoint, const AccountId& account, UnaAmount amount, uint64_t height);

    /// Advance every tracked deposit's lifecycle against a new tip
    /// height. Returns the count of stage transitions performed.
    int tipChanged(uint64_t tip_height);

    /// Mark a deposit reverted. Closes credited/settled positions
    /// and opens a compensating debit.
    void revert(const OutpointId& outpoint, UnaAmount operator_loss = 0);

   private:
    int advance(TrackedDeposit& dep, uint64_t confs);
    LedgerTimestamp now();

    Ledger* ledger_{nullptr};
    ConfirmationPolicy policy_;
    bool shadow_mode_{false};
    std::unordered_map<OutpointId, TrackedDeposit> tracked_;
};

}  // namespace dinero::vault
