#pragma once
#include "wallet/runtime_index_delivery.h"
#include "wallet/runtime_account_replay.h"
#include <functional>

namespace dinero {
class ChainstateService;

// Completion of a captured source prefix for these two stores only. It is not
// wallet readiness: account/note/vault and other consumers remain independent.
struct RuntimeTransparentRecoveryResult {
    RuntimeIndexProgress applied;
    RuntimeOutboxCursor observed_head;
};
struct RuntimeWalletRecoveryResult {
    RuntimeIndexProgress applied;
    uint64_t account_revision;
    RuntimeOutboxCursor observed_head;
};

class RuntimeWalletRecovery {
public:
    // Resume already enrolled stores. Missing/invalidated progress requires
    // baseline reconciliation; this method never adopts a baseline or enrolls
    // a store. The service and wallet/index must outlive this synchronous call.
    // No wallet lease may be held by the caller: source reads precede leases.
    // Throws on failure; committed per-store prefixes remain available to retry.
    static RuntimeTransparentRecoveryResult ResumeTransparentStores(
        ChainstateService&, WalletManager&, UTXOIndex&, uint64_t expected_session);
    // Recover already enrolled transparent stores AND one encrypted Orchard
    // account to one captured prefix. Other consumers/baseline readiness are
    // separate obligations. Chain capture happens before wallet ownership.
    static RuntimeWalletRecoveryResult ResumeWalletStores(
        ChainstateService&,WalletManager&,UTXOIndex&,uint64_t expected_session,uint32_t account);
private:
    friend struct RuntimeWalletRecoveryTestAccess;
    using Source = std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>;
    static RuntimeTransparentRecoveryResult Resume(
        const Source&, WalletManager&, UTXOIndex&, uint64_t expected_session);
    static RuntimeWalletRecoveryResult ResumeAccount(
        const RuntimeAccountReplay&,const Source&,WalletManager&,UTXOIndex&,uint64_t,uint32_t);
};
} // namespace dinero
