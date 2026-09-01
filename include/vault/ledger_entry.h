// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — append-only ledger entry types. Daemon-side C++
// port of `Core/Vault/LedgerEntry.swift`.
//
// Every state change in the vault is one of these entries. Balances
// are derived by replay; the ledger itself stores nothing else.
// Once persisted, an entry is never mutated.

#pragma once

#include "vault/vault_types.h"

#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace dinero::vault {

/// One ledger entry. Append-only; never mutated after write.
///
/// Each variant alternative encodes the lifecycle from the design
/// doc §3 (deposit_observed → credit_opened → credit_settled /
/// credit_reverted) plus the withdrawal lifecycle from §7 and the
/// operator-side controls from §6.1.

struct DepositObserved {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;
    UnaAmount amount{0};

    bool operator==(const DepositObserved&) const = default;
};

struct CreditOpened {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;
    UnaAmount amount{0};

    bool operator==(const CreditOpened&) const = default;
};

struct CreditSettled {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;

    bool operator==(const CreditSettled&) const = default;
};

struct CreditReverted {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;

    bool operator==(const CreditReverted&) const = default;
};

struct WithdrawalInitiated {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId request;
    UnaAmount amount{0};
    BackendId backend;

    bool operator==(const WithdrawalInitiated&) const = default;
};

struct WithdrawalSettled {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId request;

    bool operator==(const WithdrawalSettled&) const = default;
};

struct WithdrawalReverted {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId request;

    bool operator==(const WithdrawalReverted&) const = default;
};

struct CompensatingDebit {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;
    UnaAmount amount{0};
    /// Portion of `amount` that the user CANNOT absorb (their
    /// pending+confirmed has already been spent through a
    /// withdrawal). The remainder is operator-side loss.
    UnaAmount operatorLoss{0};

    bool operator==(const CompensatingDebit&) const = default;
};

/// Account-to-account move of already-settled funds inside the vault.
/// Atomic by construction: one entry carries both legs, so a replay can
/// never observe a half-applied transfer.
///
/// Only `confirmed` balance moves (see `LedgerAccount::transferable()`).
/// `pending` credits are operator-at-risk — a reorg lands a
/// CompensatingDebit on the account that received the deposit — so they
/// must not be movable out from under that debit. Because open credits
/// are untouched, a transfer is cap-neutral by design.
struct InternalTransfer {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId from;
    AccountId to;
    UnaAmount amount{0};

    bool operator==(const InternalTransfer&) const = default;
};

struct PolicyAdjustment {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    /// `nullopt` means a global policy change (rate caps, etc.) not
    /// tied to one user.
    std::optional<AccountId> account;
    std::string note;
    /// Signed delta — positive credits the user, negative debits.
    int64_t deltaUserBalance{0};
    /// Signed delta on operator float (admin-only, audited).
    int64_t deltaOperatorFloat{0};

    bool operator==(const PolicyAdjustment&) const = default;
};

using LedgerEntry = std::variant<
    DepositObserved,
    CreditOpened,
    CreditSettled,
    CreditReverted,
    WithdrawalInitiated,
    WithdrawalSettled,
    WithdrawalReverted,
    CompensatingDebit,
    PolicyAdjustment,
    InternalTransfer
>;

/// Pull the sequence number out of any entry shape.
inline LedgerSeq entrySeq(const LedgerEntry& e) {
    return std::visit([](const auto& concrete) { return concrete.seq; }, e);
}

/// Pull the timestamp out of any entry shape.
inline LedgerTimestamp entryTimestamp(const LedgerEntry& e) {
    return std::visit([](const auto& concrete) { return concrete.at; }, e);
}

/// AccountId this entry touches. `nullopt` for global policy
/// adjustments that don't target a specific user (PolicyAdjustment's
/// `account` is itself optional; every other variant has a
/// concrete AccountId).
///
/// InternalTransfer touches TWO accounts and reports the debited side
/// (`from`). Callers that need both legs must match the variant
/// directly rather than going through this helper.
inline std::optional<AccountId> entryAccount(const LedgerEntry& e) {
    return std::visit([](const auto& concrete) -> std::optional<AccountId> {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, InternalTransfer>) {
            return concrete.from;
        } else {
            return concrete.account;
        }
    }, e);
}

}  // namespace dinero::vault
