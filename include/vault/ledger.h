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
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dinero::vault {

class LedgerStore;

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
        // Appended after the Swift-parity set above: internal transfers
        // have no Swift counterpart, so these two kinds are C++-only.
        // Existing enumerators keep their ordinals.
        TRANSFER_INVALID,
        INSUFFICIENT_TRANSFERABLE_BALANCE,
    };

    LedgerError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

class Ledger {
   public:
    /// `store` (optional) receives every entry that passes validation,
    /// write-ahead of the in-memory apply. Pass nullptr (the default)
    /// for a memory-only ledger.
    ///
    /// NOTE: do NOT pass a store you are about to replay from — replay
    /// goes through append() and would re-persist every entry, doubling
    /// the log on each restart. Replay store-less, then `setStore`.
    explicit Ledger(LedgerCaps caps = LedgerCaps::unbounded(), LedgerStore* store = nullptr)
        : caps_{caps}, store_{store} {}

    /// Append one entry, validating invariants. On error, ledger
    /// state is unchanged (validation runs before any mutation).
    void append(LedgerEntry entry);

    /// Replay a sequence of entries onto an empty ledger. Used at
    /// startup from persisted log + tests of determinism.
    static Ledger replay(const std::vector<LedgerEntry>& entries,
                         const LedgerCaps& caps = LedgerCaps::unbounded());

    [[nodiscard]] const std::vector<LedgerEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::unordered_map<AccountId, LedgerAccount>& accounts() const noexcept {
        return accounts_;
    }
    [[nodiscard]] UnaAmount totalOpenCredits() const noexcept { return totalOpenCredits_; }
    [[nodiscard]] UnaAmount totalOperatorLoss() const noexcept { return totalOperatorLoss_; }
    [[nodiscard]] LedgerSeq nextSeq() const noexcept { return nextSeq_; }
    [[nodiscard]] const LedgerCaps& caps() const noexcept { return caps_; }

    /// Attach (or detach, with nullptr) the write-ahead store. Intended
    /// for the replay-then-attach startup sequence described above.
    void setStore(LedgerStore* store) noexcept { store_ = store; }
    [[nodiscard]] LedgerStore* store() const noexcept { return store_; }

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

    std::vector<LedgerEntry> entries_;
    std::unordered_map<AccountId, LedgerAccount> accounts_;
    std::unordered_map<AccountId, UnaAmount> openCreditsByAccount_;
    UnaAmount totalOpenCredits_{0};
    UnaAmount totalOperatorLoss_{0};
    LedgerCaps caps_;
    /// Optional write-ahead persistence sink. Not owned.
    LedgerStore* store_{nullptr};
    /// Strict-monotonic sequence head. Next entry must have
    /// `seq >= nextSeq_`. When `nextSeq_ == 0`, no entries written.
    LedgerSeq nextSeq_{0};
};

}  // namespace dinero::vault
