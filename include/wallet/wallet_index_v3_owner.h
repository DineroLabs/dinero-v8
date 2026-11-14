#pragma once

#include "wallet/wallet_manager.h"
#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace dinero {

// A v3 coin operation must hold a current DatabaseLease while it binds this
// token. Retaining the token across wallet.open/close is permitted but makes
// every subsequent operation refuse; it never follows a mutable active wallet.
class WalletIndexV3OwnerToken final {
public:
    static WalletIndexV3OwnerToken Capture(const WalletManager::DatabaseLease& lease);
    WalletIndexV3OwnerToken(const WalletIndexV3OwnerToken&) = delete;
    WalletIndexV3OwnerToken& operator=(const WalletIndexV3OwnerToken&) = delete;
    WalletIndexV3OwnerToken(WalletIndexV3OwnerToken&&) = default;
    WalletIndexV3OwnerToken& operator=(WalletIndexV3OwnerToken&&) = delete;

    void RequireCurrent(const WalletManager::DatabaseLease& lease) const;
    void BindCurrentIdentity(sqlite3_stmt* statement, int parameter,
                             const WalletManager::DatabaseLease& lease) const;

private:
    explicit WalletIndexV3OwnerToken(const WalletManager::DatabaseLease& lease);
    std::array<uint8_t,32> identity_{};
    uint64_t session_ = 0;
    std::string wallet_name_;
    std::weak_ptr<const void> instance_;
    std::thread::id thread_ = std::this_thread::get_id();
};

} // namespace dinero
