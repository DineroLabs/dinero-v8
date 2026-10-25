#pragma once
#include "daemon/reopen_rpc_gate.h"
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <utility>

namespace dinero {
class DaemonApp;
class OrchardSnapshotStore;
namespace daemon{class DatadirGuard;}
// A checked, revocable borrow of a genuine closed lifecycle intent. It grants
// no state authority. The storage library need not link a daemon/SDK to inspect
// it. Expired/resumed receipts, live App readers and foreign threads refuse.
class ClosedCoreReopenIntent final {
public:
    void Check(const daemon::DatadirGuard& guard)const{check_(guard);}
    const std::string& WalletName()const{return wallet_;}
    const std::string& CandidateJournal()const{return journal_;}
private:
    friend class CoreReopenReceipt;
    std::function<void(const daemon::DatadirGuard&)> check_;
    std::string wallet_,journal_;
    ClosedCoreReopenIntent(std::function<void(const daemon::DatadirGuard&)> check,std::string wallet,std::string journal)
        :check_(std::move(check)),wallet_(std::move(wallet)),journal_(std::move(journal)){}
};
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
    ClosedCoreReopenIntent ClosedForHandoff(const daemon::DatadirGuard&)const;
    struct Readmission {uint64_t candidates=0,resolved=0,retained=0;};
private:
    friend class DaemonApp;
    friend class OrchardSnapshotStore;
    // Conversion requires a live checked lifecycle receipt AND destruction
    // of all old App readers, not merely a stopped-but-retained App.
    struct Data;std::shared_ptr<Data> data_;
    explicit CoreReopenReceipt(std::unique_ptr<Data>);
};
} // namespace dinero
