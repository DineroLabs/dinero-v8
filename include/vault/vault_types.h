// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — value types.
//
// Daemon-side port of the Swift reference implementation in
// dinero-dpi/DineroDPI/Core/Vault. The Swift code is the executable
// spec; this header is the C++ source of truth that will eventually
// retire the Swift state machine on iOS once `vaultd` ships and
// DineroDPI cuts over to RPC clients.
//
// See LIQUIDITY_VAULT_DESIGN.md in the dinero-dpi repo for the
// design narrative.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace dinero::vault {

/// Stable per-user identity. One DineroDPI install / Qt wallet =
/// one AccountId. Internal vault accounting keys off this.
struct AccountId {
    std::string raw;

    bool operator==(const AccountId& other) const noexcept { return raw == other.raw; }
    bool operator!=(const AccountId& other) const noexcept { return !(*this == other); }
    bool operator<(const AccountId& other) const noexcept { return raw < other.raw; }
};

/// Identifier for a signing backend (operator-managed hot wallet,
/// HSM, multisig, etc.). Recorded on every ledger entry that
/// requires backend action so the audit trail attributes
/// settlements correctly across backend rotation.
struct BackendId {
    std::string raw;

    bool operator==(const BackendId& other) const noexcept { return raw == other.raw; }
    bool operator!=(const BackendId& other) const noexcept { return !(*this == other); }
};

/// `(txid, vout)` keying a deposit or withdrawal lifecycle. txid
/// stored as RAW byte order (consensus form) — display-order
/// conversion happens at the UI/RPC boundary, never inside the
/// ledger.
struct OutpointId {
    std::array<uint8_t, 32> txid_raw{};
    uint32_t vout{0};

    bool operator==(const OutpointId& other) const noexcept {
        return txid_raw == other.txid_raw && vout == other.vout;
    }
    bool operator!=(const OutpointId& other) const noexcept { return !(*this == other); }
};

/// Monetary amount in `una`. 1 DIN = 100,000,000 una. Never satoshi.
using UnaAmount = uint64_t;

/// Strict-monotonic ledger sequence. Replay is deterministic in
/// this order.
using LedgerSeq = uint64_t;

/// Unix-nanos timestamp for ledger entries. Daemon-side we use
/// nanoseconds since epoch from `std::chrono::system_clock` so the
/// audit trail has sub-millisecond precision under load.
using LedgerTimestamp = int64_t;

/// Per-deployment caps from the design doc §5.3. All values are
/// upper bounds on outstanding (`credit_opened` minus
/// `credit_settled` minus `credit_reverted`) totals at the moment
/// a new credit is opened.
struct LedgerCaps {
    /// Maximum credit for a single in-flight deposit.
    UnaAmount per_deposit{UINT64_MAX};
    /// Maximum cumulative outstanding credits per account.
    UnaAmount per_user{UINT64_MAX};
    /// Maximum cumulative outstanding credits across all accounts.
    /// MUST be ≪ operator float reserve (design doc §9).
    UnaAmount global{UINT64_MAX};

    bool operator==(const LedgerCaps& other) const noexcept {
        return per_deposit == other.per_deposit &&
               per_user == other.per_user &&
               global == other.global;
    }

    static LedgerCaps unbounded() {
        return LedgerCaps{};
    }
};

}  // namespace dinero::vault

// Hash specialisations so AccountId / OutpointId / BackendId can key
// std::unordered_map. Definition rules require these to live in
// std:: directly.
namespace std {

template <>
struct hash<dinero::vault::AccountId> {
    size_t operator()(const dinero::vault::AccountId& a) const noexcept {
        return hash<string>{}(a.raw);
    }
};

template <>
struct hash<dinero::vault::BackendId> {
    size_t operator()(const dinero::vault::BackendId& b) const noexcept {
        return hash<string>{}(b.raw);
    }
};

template <>
struct hash<dinero::vault::OutpointId> {
    size_t operator()(const dinero::vault::OutpointId& op) const noexcept {
        size_t h = 0;
        for (auto byte : op.txid_raw) {
            h = h * 31 + byte;
        }
        return h ^ (static_cast<size_t>(op.vout) * 2654435761U);
    }
};

}  // namespace std
