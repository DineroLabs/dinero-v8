#pragma once
#include "wallet/transaction_signer.h"
#include <cstdint>
#include <string>
namespace dinero {
class WalletManager;
// An identity snapshot, not a key or lasting authorization. Signing rechecks it.
struct WalletSigningIdentity {std::string name;uint64_t session=0;};
WalletSigningIdentity CaptureWalletSigningIdentity(WalletManager&,const std::string& requested_name);
// Caller resolves chain/index data before entry. No admission, broadcast or
// chain callbacks occur while the selected wallet/key owner is held here.
SignResult SignWalletTransaction(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&);
}
