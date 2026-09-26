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

RuntimeWalletRecoveryResult RuntimeWalletRecovery::ResumeAccount(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,
        UTXOIndex& index,uint64_t session,uint32_t account_number) {
    using Account=wallet::OrchardAccountDelivery;
    {const auto lease=wallet.AcquireDatabaseLease();Require(wallet.database_leases_==1,
        "Wallet recovery requires released caller lease");}
    const auto& first=view.Event(1);const auto target=view.Head();
    const Account::Profile profile{first.context.domain,first.context.activation_height,account_number};
    struct Snapshot {RuntimeIndexProgress indexed,ordinary;Account::Applied account;};
    const auto read=[&] {
        const auto lease=wallet.AcquireDatabaseLease();const auto stores=ReadStores(wallet,index,session);
        auto account=Account::ReadForReplay(wallet,session,profile,view);
        Require(account.account.Delivery().sequence,"Wallet recovery account baseline reconciliation required");
        return Snapshot{stores.first,stores.second,std::move(account)};
    };
    auto current=read();
    const auto account_cursor=[](const Snapshot& s) {
        const auto& d=s.account.account.Delivery();return RuntimeOutboxCursor{d.sequence,d.digest};
    };
    const auto unchanged=[&](const Snapshot& a,const Snapshot& b) {
        return Same(a.indexed,b.indexed)&&Same(a.ordinary,b.ordinary)&&a.account.revision==b.account.revision&&
            account_cursor(a)==account_cursor(b);
    };
    const auto origin=view.Point({}).checkpoint;
    for(const auto* p:{&current.indexed,&current.ordinary}) {
        Require(p->origin_hash==origin.block_hash&&p->origin_height==origin.height,
            "Wallet recovery source origin mismatch");
        const auto point=view.Point(p->cursor).checkpoint;
        Require(p->tip_hash==point.block_hash&&p->tip_height==point.height,"Wallet recovery source position mismatch");
        CheckPosition(*p,source(p->cursor,1));
    }
    const auto initial_account=account_cursor(current);
    const auto position=source(initial_account,1);
    const auto& scan=current.account.account.Scan().Checkpoint();
    Require(position.after_tip&&position.after_tip->first==scan.block_hash&&position.after_tip->second==scan.height,
        "Wallet recovery account source position mismatch");
    auto sequence=std::min({current.indexed.cursor.sequence,current.ordinary.cursor.sequence,initial_account.sequence});
    for(++sequence;sequence<=target.sequence;++sequence) {
        const auto& event=view.Event(sequence);
        const auto lease=wallet.AcquireDatabaseLease();
        Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
        // Every committed prefix is retained. Failure of a later store never
        // acknowledges it, erases earlier receipts, or replays an ahead store.
        if(current.indexed.cursor.sequence<sequence)
            current.indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,event);
        if(current.ordinary.cursor.sequence<sequence)
            current.ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,event);
        if(account_cursor(current).sequence<sequence) {
            const auto before=view.Point(account_cursor(current));
            if(!event.IsOrchardProfile())
                current.account=Account::Historical(wallet,session,profile,current.account.revision,before,event);
            else if(event.direction==RuntimeBlockDirection::Connect)
                current.account=Account::Connect(wallet,session,profile,current.account.revision,before,event,
                    view.Block(sequence),view.State(sequence),view.Authorizations(sequence));
            else
                current.account=Account::Disconnect(wallet,session,profile,current.account.revision,before,event,
                    view.Block(sequence),view.Point(event.cursor));
            Require(account_cursor(current)==event.cursor&&current.account.account.Scan().Checkpoint()==view.Point(event.cursor).checkpoint,
                "Wallet recovery account applied position mismatch");
        }
    }
    Require(current.indexed.cursor==target&&current.ordinary.cursor==target&&account_cursor(current)==target&&
        Same(current.indexed,current.ordinary),"Wallet recovery captured head mismatch");
    const auto final=source(target,1);CheckPosition(current.indexed,final);
    Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
    return {current.indexed,current.account.revision,final.head};
}
} // namespace dinero
