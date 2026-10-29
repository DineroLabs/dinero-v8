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
#include "vault/credit_allocation.h"

#include <optional>
#include <string>
#include <variant>
#include <type_traits>

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

// Reinstates one fully compensated reversal after the service has verified
// canonical inclusion and settlement maturity. Amounts are derived from the
// referenced ledger history; callers cannot supply a refund or operator loss.
struct CreditReinstated {
    LedgerSeq seq{0};
    LedgerTimestamp at{0};
    AccountId account;
    OutpointId deposit;
    LedgerSeq reversalSeq{0};
    LedgerSeq compensationSeq{0};
    bool operator==(const CreditReinstated&) const = default;
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

// Versioned principal attribution entries. Old alternatives and their wire tags
// keep their existing order and replay semantics. Encoding uses DNVS06; live canonical settlement wiring is still WIP.
// Reserve records selected source identities before any dispatch callback. A
// later payment binding never selects different funding origins on retry.
struct WithdrawalAllocationReserved {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{}; UnaAmount amount{0};
    std::vector<CreditAllocationRef> sources;
    bool operator==(const WithdrawalAllocationReserved&) const = default;
};
struct WithdrawalAllocationDispatchStarted {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{};
    bool operator==(const WithdrawalAllocationDispatchStarted&) const = default;
};
struct WithdrawalAllocationPaymentBound {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{}; AllocationPayment payment; BackendId backend;
    bool operator==(const WithdrawalAllocationPaymentBound&) const = default;
};
struct WithdrawalAllocationIncluded {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{}; AllocationInclusion inclusion;
    bool operator==(const WithdrawalAllocationIncluded&) const = default;
};
struct WithdrawalAllocationDisconnected {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{}; AllocationInclusion inclusion;
    bool operator==(const WithdrawalAllocationDisconnected&) const = default;
};
struct WithdrawalAllocationReleased {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    AllocationRequestId request{};
    bool operator==(const WithdrawalAllocationReleased&) const = default;
};
struct CreditPositionMatured {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    LedgerSeq credit_seq{0}; OutpointId deposit;
    bool operator==(const CreditPositionMatured&) const = default;
};
struct CreditPositionReverted {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    LedgerSeq credit_seq{0}; OutpointId deposit;
    bool operator==(const CreditPositionReverted&) const = default;
};
struct CreditPositionRestored {
    LedgerSeq seq{0}; LedgerTimestamp at{0}; AccountId account;
    LedgerSeq credit_seq{0}; OutpointId deposit;
    bool operator==(const CreditPositionRestored&) const = default;
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
    CreditReinstated,
    WithdrawalAllocationReserved,
    WithdrawalAllocationDispatchStarted,
    WithdrawalAllocationPaymentBound,
    WithdrawalAllocationIncluded,
    WithdrawalAllocationDisconnected,
    WithdrawalAllocationReleased,
    CreditPositionMatured,
    CreditPositionReverted,
    CreditPositionRestored
>;

inline bool IsCreditAllocationEntry(const LedgerEntry& entry) {
    return std::visit([](const auto& value) {
        using T=std::decay_t<decltype(value)>;
        return std::is_same_v<T,WithdrawalAllocationReserved> || std::is_same_v<T,WithdrawalAllocationDispatchStarted> || std::is_same_v<T,WithdrawalAllocationPaymentBound> || std::is_same_v<T,WithdrawalAllocationIncluded> || std::is_same_v<T,WithdrawalAllocationDisconnected> || std::is_same_v<T,WithdrawalAllocationReleased> || std::is_same_v<T,CreditPositionMatured> || std::is_same_v<T,CreditPositionReverted> || std::is_same_v<T,CreditPositionRestored>;
    },entry);
}

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
inline std::optional<AccountId> entryAccount(const LedgerEntry& e) {
    return std::visit([](const auto& concrete) -> std::optional<AccountId> {
        return concrete.account;
    }, e);
}

}  // namespace dinero::vault
