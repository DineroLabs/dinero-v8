// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — coordinator + replay engine for the internal
// ledger. Daemon-side port of `Core/Vault/Ledger.swift`.
//
// The Ledger:
//   - holds the append-only entry log
//   - derives per-account state by replay
//   - enforces the four invariants from the design doc §6.2 on
//     every append
//   - tracks operator-float utilization for cap enforcement
//
// Single-writer; multi-reader is the responsibility of the
// surrounding service. Persistence to LevelDB (matching the
// monotonic-seq layout the Swift Ledger requires) is added by
// `vault/ledger_store.cpp` in C.1's persistence pass.

#pragma once

#include "vault/ledger_account.h"
#include "vault/ledger_entry.h"
#include "vault/vault_types.h"

#include <stdexcept>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dinero::vault {

/// Errors produced by `append`. Mapped to the Swift `LedgerError`
/// enum case-for-case so the gtest port matches the Swift
/// LedgerReplayTests.
class LedgerError : public std::runtime_error {
   public:
    enum class Kind : uint8_t {
        SEQUENCE_NOT_MONOTONIC,
        OPEN_CREDITS_EXCEED_CAP,
        LIFECYCLE_INCONSISTENT,
        PER_USER_CAP_EXCEEDED,
        DEPOSIT_LIFECYCLE_CLOSED,
        SEQUENCE_EXHAUSTED,
        ARITHMETIC_OVERFLOW,
    };

    LedgerError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

struct CreditReinstatement {
    LedgerSeq reversalSeq{0};
    LedgerSeq compensationSeq{0};
    UnaAmount amount{0};
    UnaAmount refund{0};
    UnaAmount operatorLoss{0};
};

class Ledger {
   public:
    explicit Ledger(LedgerCaps caps = LedgerCaps::unbounded()) : caps_{caps} {}

    /// Prepare the derived account/counter state before appending an entry.
    /// Errors preserve entries, accounts, counters and sequence. Successful
    /// publication after the entry append uses only nonthrowing swaps.
    /// This is in-memory atomicity, not a durable storage transaction.
    void append(LedgerEntry entry);

    // Append the existing reversal and compensation shapes together. Loss is
    // nominal credit minus the balance removed by the actual account transition.
    // Historical entries retain their recorded amounts when replayed.
    void revertCredit(const AccountId& account, const OutpointId& deposit,
                      LedgerTimestamp at);

    /// Replay a sequence of entries onto an empty ledger. Used at
    /// startup from persisted log + tests of determinism.
    static Ledger replay(const std::vector<LedgerEntry>& entries,
                         const LedgerCaps& caps = LedgerCaps::unbounded());

    [[nodiscard]] bool hasCreditAllocation(const AccountId& account) const noexcept {
        return allocationAccounts_.contains(account);
    }
    [[nodiscard]] const CreditAllocationState& creditAllocations() const noexcept { return allocations_; }
    // Select for a NEW request only; the returned references must be recorded
    // in WithdrawalAllocationReserved before dispatch. Never use this for retry.
    [[nodiscard]] std::vector<CreditAllocationRef> selectCreditAllocations(
        const AccountId&,UnaAmount) const;
    [[nodiscard]] LedgerSeq creditPositionSeq(const AccountId&,const OutpointId&) const;

    [[nodiscard]] const std::vector<LedgerEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::unordered_map<AccountId, LedgerAccount>& accounts() const noexcept {
        return accounts_;
    }
    [[nodiscard]] UnaAmount totalOpenCredits() const noexcept { return totalOpenCredits_; }
    [[nodiscard]] UnaAmount totalOperatorLoss() const noexcept { return totalOperatorLoss_; }
    [[nodiscard]] LedgerSeq nextSeq() const noexcept { return nextSeq_; }
    [[nodiscard]] const LedgerCaps& caps() const noexcept { return caps_; }

    // Complete, unique reversal/compensation pair for this current reverted
    // position. Reconstructed from existing entries during normal replay.
    [[nodiscard]] std::optional<CreditReinstatement> reinstatement(
        const AccountId& account, const OutpointId& deposit) const;

    /// Convenience: lookup account state, returning a default-
    /// constructed `LedgerAccount{account}` if not present. Useful
    /// for tests + UI queries.
    [[nodiscard]] LedgerAccount accountOr(const AccountId& account) const {
        auto it = accounts_.find(account);
        if (it != accounts_.end()) {
            return it->second;
        }
        return LedgerAccount{account};
    }

   private:
    void validate(const LedgerEntry& entry);
    void applyToAccounts(const LedgerEntry& entry);
    LedgerAccount& ensureAccount(const AccountId& account);
    CreditAllocationState captureUnambiguousCreditOrigins(const AccountId&) const;
    bool applyAllocationEntry(const LedgerEntry&);
    void syncAllocatedAccount(const AccountId&);
    CreditAllocationState allocations_;
    std::set<AccountId> allocationAccounts_;

    struct Reversal {
        CreditReinstatement values;
        bool compensated{false};
        bool usable{true};
    };
    std::unordered_map<AccountId,std::unordered_map<OutpointId,Reversal>> reversals_;
    std::vector<LedgerEntry> entries_;
    std::unordered_map<AccountId, LedgerAccount> accounts_;
    std::unordered_map<AccountId, UnaAmount> openCreditsByAccount_;
    UnaAmount totalOpenCredits_{0};
    UnaAmount totalOperatorLoss_{0};
    LedgerCaps caps_;
    /// Strict-monotonic sequence head. Next entry must have
    /// `seq >= nextSeq_`. Zero is valid initially; UINT64_MAX is an
    /// exhausted next value and cannot be used for another append.
    LedgerSeq nextSeq_{0};
};

}  // namespace dinero::vault
