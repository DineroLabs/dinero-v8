#include "wallet/wallet_index_v3_owner.h"

#include <algorithm>
#include <stdexcept>

namespace dinero {

WalletIndexV3OwnerToken::WalletIndexV3OwnerToken(const WalletManager::DatabaseLease& lease)
    : identity_(lease.ReadDeliveryIdentity()), session_(lease.Session()),
      wallet_name_(lease.WalletName()), instance_(lease.InstanceToken()) {
    if (!session_ || wallet_name_.empty() || instance_.expired() ||
        std::all_of(identity_.begin(),identity_.end(),[](uint8_t n){return n==0;}))
        throw std::runtime_error("Wallet index v3 owner unavailable");
}

WalletIndexV3OwnerToken WalletIndexV3OwnerToken::Capture(
        const WalletManager::DatabaseLease& lease) {
    return WalletIndexV3OwnerToken(lease);
}

void WalletIndexV3OwnerToken::RequireCurrent(const WalletManager::DatabaseLease& lease) const {
    const auto instance=instance_.lock();
    if (thread_!=std::this_thread::get_id() || !instance ||
        instance!=lease.InstanceToken() || session_!=lease.Session() ||
        wallet_name_!=lease.WalletName() || identity_!=lease.ReadDeliveryIdentity())
        throw std::runtime_error("Wallet index v3 owner/session changed");
}

void WalletIndexV3OwnerToken::BindCurrentIdentity(
        sqlite3_stmt* statement, int parameter, const WalletManager::DatabaseLease& lease) const {
    RequireCurrent(lease);
    if (!statement || parameter<=0 || sqlite3_bind_blob(statement,parameter,identity_.data(),
            int(identity_.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
        throw std::runtime_error("Wallet index v3 identity bind failed");
}

} // namespace dinero
