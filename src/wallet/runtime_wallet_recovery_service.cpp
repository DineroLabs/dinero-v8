#include "wallet/runtime_wallet_recovery.h"
#include "daemon/services/chainstate_service.h"
#include "wallet/wallet_manager.h"
#include <stdexcept>

namespace dinero {
RuntimeTransparentRecoveryResult RuntimeWalletRecovery::ResumeTransparentStores(
        ChainstateService& chain, WalletManager& wallet, UTXOIndex& index, uint64_t session) {
    return Resume([&chain](RuntimeOutboxCursor cursor,size_t count) {
        const auto page=chain.getRuntimeDeliveryPage(cursor,count);
        if (!page.ok()) throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session);
}
RuntimeWalletRecoveryResult RuntimeWalletRecovery::ResumeWalletStores(
        ChainstateService& chain,WalletManager& wallet,UTXOIndex& index,uint64_t session,uint32_t account) {
    {const auto lease=wallet.AcquireDatabaseLease();
     if(wallet.database_leases_!=1)throw std::runtime_error("Wallet recovery requires released caller lease");}
    const auto view=chain.getRuntimeAccountReplay();
    if(!view.ok())throw std::runtime_error("Wallet recovery checked account source unavailable");
    return ResumeAccount(**view,[&chain](RuntimeOutboxCursor cursor,size_t count){
        const auto page=chain.getRuntimeDeliveryPage(cursor,count);
        if(!page.ok())throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session,account);
}
} // namespace dinero
