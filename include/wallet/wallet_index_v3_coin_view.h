#pragma once

#include "wallet/wallet_index_v3_owner.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sqlite3.h>
#include <string>

namespace dinero {

// Raw, owner-scoped v3 storage inspection. A returned row is not a wallet
// balance or a delivery receipt: financial callers still require the exact
// selected-source ordinary/index receipts and a proved ownership inventory.
// The wallet lease must be acquired before calling into this index (never the
// reverse), and is held until the query completes.
class WalletIndexV3CoinView final {
public:
    explicit WalletIndexV3CoinView(const std::filesystem::path& index_file);
    ~WalletIndexV3CoinView();
    WalletIndexV3CoinView(const WalletIndexV3CoinView&) = delete;
    WalletIndexV3CoinView& operator=(const WalletIndexV3CoinView&) = delete;

    std::optional<uint64_t> ReadRawCoinValue(
        const WalletIndexV3OwnerToken& owner,
        const WalletManager::DatabaseLease& lease,
        const std::string& txid_hex, uint32_t vout) const;
    uint64_t CountRawUnspent(
        const WalletIndexV3OwnerToken& owner,
        const WalletManager::DatabaseLease& lease) const;

private:
    sqlite3* db_ = nullptr;
    int64_t schema_version_ = -1;
    mutable std::mutex mutex_;
};

} // namespace dinero
