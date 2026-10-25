#pragma once
#include "daemon/reopen_rpc_gate.h"
#include <cstdint>
#include <memory>
#include <string>

namespace dinero {
class DaemonApp;
namespace daemon{class DatadirGuard;}
// Host lifecycle receipt, NOT a catalog/install/scan capability. Minted only
// after a held-root, drained, sealed current mempool capture. The host must
// destroy the stopped App (all old forest readers) before initializing a fresh
// App under the SAME retained root guard and this gate. Abandoning a committed
// receipt keeps RPC paused, rather than releasing semantic queue errors.
class CoreReopenReceipt final {
public:
    ~CoreReopenReceipt();
    CoreReopenReceipt(const CoreReopenReceipt&)=delete;
    CoreReopenReceipt& operator=(const CoreReopenReceipt&)=delete;
    std::shared_ptr<ReopenRpcGate> Gate()const;
    const std::string& WalletName()const;
    const std::string& CandidateJournal()const;
    // Mandatory before replacement Init; pins the retained journal/root
    // identity again, without granting state-conversion authority.
    void CheckBeforeInit(const daemon::DatadirGuard&)const;
    struct Readmission {uint64_t candidates=0,resolved=0,retained=0;};
private:
    friend class DaemonApp;
    struct Data;std::unique_ptr<Data> data_;
    explicit CoreReopenReceipt(std::unique_ptr<Data>);
};
} // namespace dinero
