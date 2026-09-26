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
    const auto result=ResumeAccounts(view,source,wallet,index,session,account_number);
    return {result.applied,result.account_revisions.front().second,result.observed_head};
}

RuntimeEnrolledWalletRecoveryResult RuntimeWalletRecovery::ResumeAccounts(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,
        UTXOIndex& index,uint64_t session,std::optional<uint32_t> selected_account) {
    using Account=wallet::OrchardAccountDelivery;
    {const auto lease=wallet.AcquireDatabaseLease();Require(wallet.database_leases_==1,
        "Wallet recovery requires released caller lease");}
    const auto& first=view.Event(1);const auto target=view.Head();
    struct Snapshot {RuntimeIndexProgress indexed,ordinary;std::vector<Account::Enrolled> accounts;};
    const auto read=[&] {
        const auto lease=wallet.AcquireDatabaseLease();const auto stores=ReadStores(wallet,index,session);
        std::vector<Account::Enrolled> accounts;
        if(selected_account){
            const Account::Profile profile{first.context.domain,first.context.activation_height,*selected_account};
            auto account=Account::ReadForReplay(wallet,session,profile,view);
            Require(account.account.Delivery().sequence,"Wallet recovery account baseline reconciliation required");
            accounts.push_back({*selected_account,std::move(account)});
        }else accounts=Account::ReadEnrolledForReplay(wallet,session,view);
        return Snapshot{stores.first,stores.second,std::move(accounts)};
    };
    auto current=read();
    const auto account_cursor=[](const Account::Enrolled& s) {
        const auto& d=s.state.account.Delivery();return RuntimeOutboxCursor{d.sequence,d.digest};
    };
    const auto unchanged=[&](const Snapshot& a,const Snapshot& b) {
        if(!Same(a.indexed,b.indexed)||!Same(a.ordinary,b.ordinary)||a.accounts.size()!=b.accounts.size())return false;
        for(size_t i=0;i<a.accounts.size();++i)
            if(a.accounts[i].number!=b.accounts[i].number||a.accounts[i].state.revision!=b.accounts[i].state.revision||
               account_cursor(a.accounts[i])!=account_cursor(b.accounts[i]))return false;
        return true;
    };
    const auto origin=view.Point({}).checkpoint;
    for(const auto* p:{&current.indexed,&current.ordinary}) {
        Require(p->origin_hash==origin.block_hash&&p->origin_height==origin.height,
            "Wallet recovery source origin mismatch");
        const auto point=view.Point(p->cursor).checkpoint;
        Require(p->tip_hash==point.block_hash&&p->tip_height==point.height,"Wallet recovery source position mismatch");
        CheckPosition(*p,source(p->cursor,1));
    }
    auto sequence=std::min(current.indexed.cursor.sequence,current.ordinary.cursor.sequence);
    // Validate every account, including an ahead account, before any effects.
    for(const auto& entry:current.accounts){
        const auto cursor=account_cursor(entry);const auto position=source(cursor,1);
        const auto& scan=entry.state.account.Scan().Checkpoint();
        Require(position.after_tip&&position.after_tip->first==scan.block_hash&&position.after_tip->second==scan.height,
            "Wallet recovery account source position mismatch");
        sequence=std::min(sequence,cursor.sequence);
    }
    for(++sequence;sequence<=target.sequence;++sequence) {
        const auto& event=view.Event(sequence);
        const auto lease=wallet.AcquireDatabaseLease();
        Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
        if(current.indexed.cursor.sequence<sequence)
            current.indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,event);
        if(current.ordinary.cursor.sequence<sequence)
            current.ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,event);
        // Accounts commit in ascending account-number order. A failure retains
        // all earlier store/account prefixes for the next owned recovery pass.
        for(auto& entry:current.accounts)if(account_cursor(entry).sequence<sequence) {
            const Account::Profile profile{first.context.domain,first.context.activation_height,entry.number};
            auto& account=entry.state;const auto before=view.Point(account_cursor(entry));
            if(!event.IsOrchardProfile())
                account=Account::Historical(wallet,session,profile,account.revision,before,event);
            else if(event.direction==RuntimeBlockDirection::Connect)
                account=Account::Connect(wallet,session,profile,account.revision,before,event,
                    view.Block(sequence),view.State(sequence),view.Authorizations(sequence));
            else
                account=Account::Disconnect(wallet,session,profile,account.revision,before,event,
                    view.Block(sequence),view.Point(event.cursor));
            Require(account_cursor(entry)==event.cursor&&account.account.Scan().Checkpoint()==view.Point(event.cursor).checkpoint,
                "Wallet recovery account applied position mismatch");
        }
    }
    Require(current.indexed.cursor==target&&current.ordinary.cursor==target&&Same(current.indexed,current.ordinary),
        "Wallet recovery captured head mismatch");
    for(const auto& entry:current.accounts)Require(account_cursor(entry)==target,"Wallet recovery captured head mismatch");
    const auto final=source(target,1);CheckPosition(current.indexed,final);
    Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
    std::vector<std::pair<uint32_t,uint64_t>> revisions;
    for(const auto& entry:current.accounts)revisions.emplace_back(entry.number,entry.state.revision);
    return {current.indexed,std::move(revisions),final.head};
}
} // namespace dinero
