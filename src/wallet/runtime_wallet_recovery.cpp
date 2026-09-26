#include "wallet/runtime_wallet_recovery.h"
#include "wallet/wallet_manager.h"
#include <algorithm>
#include <stdexcept>

namespace dinero {
namespace {
bool Same(const RuntimeIndexProgress& a, const RuntimeIndexProgress& b) {
    return a.cursor==b.cursor && a.origin_hash==b.origin_hash &&
        a.origin_height==b.origin_height && a.tip_hash==b.tip_hash && a.tip_height==b.tip_height;
}
void Require(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}
auto ReadStores(WalletManager& wallet, UTXOIndex& index, uint64_t session) {
    const auto lease=wallet.AcquireDatabaseLease();
    Require(lease->Session()==session,"Wallet recovery selection changed");
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(wallet,index,session);
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(wallet,session);
    Require(indexed.has_value() && ordinary.has_value(),"Wallet recovery baseline reconciliation required");
    return std::pair{*indexed,*ordinary};
}
void CheckPosition(const RuntimeIndexProgress& progress, const RuntimeOutboxPage& page) {
    Require(page.after_tip.has_value() && page.after_tip->first==progress.tip_hash &&
        page.after_tip->second==progress.tip_height,"Wallet recovery source position mismatch");
}
}

RuntimeTransparentRecoveryResult RuntimeWalletRecovery::Resume(
        const Source& source, WalletManager& wallet, UTXOIndex& index, uint64_t session) {
    {
        const auto lease=wallet.AcquireDatabaseLease();
        Require(wallet.database_leases_==1,"Wallet recovery requires released caller lease");
    }
    // Snapshot both receipts under one wallet ownership interval, then release
    // ownership before acquiring the selected-chain source lock.
    auto [indexed,ordinary]=ReadStores(wallet,index,session);
    const auto origin=source({},1);
    Require(!origin.events.empty(),"Wallet recovery source origin unavailable");
    const auto& first=origin.events.front();
    const bool connect=first.direction==RuntimeBlockDirection::Connect;
    const auto origin_hash=connect?first.context.parent_hash:first.context.block_hash;
    const auto origin_height=connect?first.context.height-1:first.context.height;
    const auto target=origin.head;
    for (const auto* progress : {&indexed,&ordinary}) {
        Require(progress->cursor.sequence && progress->cursor.sequence<=target.sequence &&
            progress->origin_hash==origin_hash && progress->origin_height==origin_height,
            "Wallet recovery source origin mismatch");
        // Validate BOTH applied cursors, including the ahead store and EOF. A
        // matching height or agreement between stores is not source validation.
        CheckPosition(*progress,source(progress->cursor,1));
    }
    while (std::min(indexed.cursor.sequence,ordinary.cursor.sequence)<target.sequence) {
        const auto after=indexed.cursor.sequence<=ordinary.cursor.sequence?indexed.cursor:ordinary.cursor;
        const auto page=source(after,static_cast<size_t>(std::min<uint64_t>(128,target.sequence-after.sequence)));
        Require(!page.events.empty(),"Wallet recovery source ended before captured head");
        for (const auto& event : page.events) {
            Require(event.cursor.sequence<=target.sequence,"Wallet recovery page exceeds captured head");
            const auto lease=wallet.AcquireDatabaseLease();
            const auto current=ReadStores(wallet,index,session);
            Require(Same(current.first,indexed) && Same(current.second,ordinary),
                "Wallet recovery stores changed during source read");
            // Index first, ordinary second. If the second commit fails, restart
            // reads the two actual receipts and applies only the lagging store.
            if (indexed.cursor.sequence<event.cursor.sequence)
                indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,event);
            if (ordinary.cursor.sequence<event.cursor.sequence)
                ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,event);
        }
    }
    Require(indexed.cursor==target && ordinary.cursor==target && Same(indexed,ordinary),
        "Wallet recovery captured head mismatch");
    const auto final_page=source(target,1);
    CheckPosition(indexed,final_page);
    {
        const auto current=ReadStores(wallet,index,session);
        Require(Same(current.first,indexed) && Same(current.second,ordinary),
            "Wallet recovery stores changed during source read");
    }
    // The chain may already have advanced. Return the checked observation, not
    // a lasting readiness claim or a published process-wide wallet height.
    return {indexed,final_page.head};
}
} // namespace dinero
