#include "wallet/runtime_index_delivery.h"
#include "wallet/wallet_manager.h"
#include <sqlite3.h>
#include <stdexcept>
namespace dinero {
namespace {
class WalletIndexSnapshot {
    sqlite3* db_;
public:
    explicit WalletIndexSnapshot(sqlite3* db):db_(db) {
        if(!db_ || !sqlite3_get_autocommit(db_))throw std::runtime_error("Wallet index snapshot owner unavailable");
        if(sqlite3_exec(db_,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)
            throw std::runtime_error("Wallet index snapshot unavailable");
    }
    ~WalletIndexSnapshot(){if(!sqlite3_get_autocommit(db_) && sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK && !sqlite3_get_autocommit(db_))std::terminate();}
    void Commit(){if(sqlite3_exec(db_,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Wallet index snapshot commit failed");}
};
}
std::optional<RuntimeIndexProgress> RuntimeIndexDelivery::ReadForWallet(WalletManager& wallet,UTXOIndex& index,uint64_t expected_session) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(lease->Session()!=expected_session)throw std::runtime_error("Wallet delivery selection changed");
    const auto identity=lease->EnsureDeliveryIdentity();WalletIndexSnapshot snapshot(lease->Database());
    const auto historical=lease->ReadHistoricalImportScriptsInTransaction();
    auto result=Read(index,identity,&historical);snapshot.Commit();return result;
}
RuntimeIndexProgress RuntimeIndexDelivery::ApplyForWallet(WalletManager& wallet,UTXOIndex& index,uint64_t expected_session,
                                                        const RuntimeOutboxEvent& event) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(lease->Session()!=expected_session)throw std::runtime_error("Wallet delivery selection changed");
    const auto identity=lease->EnsureDeliveryIdentity();WalletIndexSnapshot snapshot(lease->Database());
    const auto historical=lease->ReadHistoricalImportScriptsInTransaction();
    return Apply(index,identity,event,nullptr,[&]{snapshot.Commit();},&historical);
}
} // namespace dinero
