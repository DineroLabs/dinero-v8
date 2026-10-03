#include "wallet/runtime_index_delivery.h"
#include "wallet/wallet_manager.h"
#include <stdexcept>
namespace dinero {
std::optional<RuntimeIndexProgress> RuntimeIndexDelivery::ReadForWallet(WalletManager& wallet, UTXOIndex& index, uint64_t expected_session) {
    const auto lease = wallet.AcquireDatabaseLease();
    if (lease->Session() != expected_session)
        throw std::runtime_error("Wallet delivery selection changed");
    return Read(index, lease->EnsureDeliveryIdentity());
}
RuntimeIndexProgress RuntimeIndexDelivery::ApplyForWallet(WalletManager& wallet, UTXOIndex& index, uint64_t expected_session,
                                                        const RuntimeOutboxEvent& event) {
    const auto lease = wallet.AcquireDatabaseLease();
    if (lease->Session() != expected_session)
        throw std::runtime_error("Wallet delivery selection changed");
    return Apply(index, lease->EnsureDeliveryIdentity(), event);
}
} // namespace dinero
