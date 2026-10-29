#pragma once
#include "vault/vault_service.h"
#include <span>

namespace dinero::vault {

// Complete present in-memory service fields. This value is neither an
// authenticated owner nor a chain/readiness certificate. Only the eventual
// authenticated store may authorize restoring it into a live service.
struct VaultSavedDeposit {
    TrackedDeposit deposit;
    std::array<uint8_t,32> observed_block{};
};
struct VaultSavedWithdrawal {
    WithdrawalRequest request;
    WithdrawalState state;
};
struct VaultStateSnapshot {
    uint64_t revision{0};
    VaultServiceConfig config;
    std::vector<LedgerEntry> entries;
    std::vector<VaultSavedDeposit> deposits;
    std::vector<VaultSavedWithdrawal> withdrawals;
};

// Versioned canonical encoding, bounded to 64 MiB. All lengths/counts and
// terminal input are checked; duplicates and noncanonical ordering refuse.
// No file creation, account invention, SQL mutation, owner generation, or
// live publication is performed here. Limits refuse rather than truncate.
std::vector<uint8_t> EncodeVaultState(const VaultStateSnapshot& state);
VaultStateSnapshot DecodeVaultState(std::span<const uint8_t> bytes);

// Replay the actual ledger and require a one-to-one binding between every
// present ledger lifecycle and its saved deposit/request. This does not
// authenticate the input or prove historical deletion/backup completeness.
// The returned ledger belongs to an unpublished candidate only.
Ledger ReplayVaultStateLedger(const VaultStateSnapshot& state);

} // namespace dinero::vault
