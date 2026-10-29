#pragma once
#include "vault/state_store.h"
#include "rpc/rpc_registry.h"
#include "wallet/wallet_transaction_signer.h"
namespace dinero::vault {
struct WalletWithdrawalDispatchState;
// Application-owned lifetime for callbacks that carry a daemon context.
// Close before stopping or destroying that context's services. Close refuses
// on an active callback's own thread, otherwise prevents new calls and waits
// for existing synchronous calls to release. No wallet/chain/SQLite locks may
// be held while closing. Destruction closes; self-destruction is a contract
// violation and terminates rather than leaving an accessible dangling context.
// Retained factories/services cannot reopen a closed owner.
class WalletWithdrawalDispatchOwner final {
public:
    WalletWithdrawalDispatchOwner(ExecutionContext,std::shared_ptr<WalletService>,
                                  WalletSigningIdentity,VaultStateDomain);
    ~WalletWithdrawalDispatchOwner();
    WalletWithdrawalDispatchOwner(const WalletWithdrawalDispatchOwner&)=delete;
    WalletWithdrawalDispatchOwner& operator=(const WalletWithdrawalDispatchOwner&)=delete;
    VaultWithdrawalDispatcherFactory Factory() const;
    void Close();
private:
    std::shared_ptr<WalletWithdrawalDispatchState> state_;
};
} // namespace dinero::vault
