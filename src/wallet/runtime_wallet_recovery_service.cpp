#include "wallet/runtime_wallet_recovery.h"
#include "daemon/services/chainstate_service.h"
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
} // namespace dinero
