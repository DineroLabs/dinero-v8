#include "consensus/utxo_publication.h"
#include "consensus/chainparams.h"
#include <cstdlib>
#include <iostream>
#include <new>

// Fail every ordinary C++ allocation on this thread during publication.
static thread_local bool refuse_allocation = false;
void* operator new(std::size_t n) {
    if (refuse_allocation) throw std::bad_alloc();
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x << '\n'; std::exit(1); } } while(false)
using namespace dinero;
using namespace dinero::consensus;
uint256 H(uint8_t n) { uint256 h; h.begin()[0]=n; return h; }
uint256 Root(const UtreexoForest& f) { uint256 h;const auto r=f.getCommitment();std::copy(r.begin(),r.end(),h.begin());return h; }
template<class F> void Rejected(F f) { bool failed=false;try { f(); } catch(const std::runtime_error&) {failed=true;} CHECK(failed); }
int main() {
    SelectParams(Chain::REGTEST);
    ConsensusUTXOSet live;
    const OutPoint spent(TxId(H(1)),0), added(TxId(H(2)),0), untouched(TxId(H(3)),1);
    const UTXOEntry a(AmountUna::Una(100),{0x51},1,false);
    const UTXOEntry b(AmountUna::Una(90),{0x51,0x51},2,false);
    const UTXOEntry ct(AmountUna::Una(0),{0x52},1,false,true,{2,3,4});
    CHECK(live.AddCoin(spent,a));CHECK(live.AddCoin(untouched,ct));
    live.SetBestBlock(H(10),1);
    const auto before=live.GetForest(); const auto before_root=Root(before);
    auto after=before;CHECK(after.add(UtreexoHash(32,42))!=UINT64_MAX);
    const auto after_root=Root(after);
    const std::vector<UTXOPublicationChange> changes{{spent,a,{}},{added,{},b}};
    const auto prepare=[&](const auto& edits) {
        return PreparedUTXOPublication::PrepareUnderLock(live,1,H(10),before_root,edits,after,2,H(11),after_root);
    };
    { auto abandoned=prepare(changes);abandoned.CheckReadyUnderLock(); }
    CHECK(live.GetBestBlock()==H(10) && live.HaveCoin(spent) && !live.HaveCoin(added));
    CHECK(live.SnapshotForestCommitment()==before.getCommitment());
    for (unsigned field=0;field<6;++field) {
        auto wrong=changes;
        auto& coin=*wrong[0].before;
        if(field==0) coin.value=AmountUna::Una(101);
        if(field==1) coin.scriptPubKey.push_back(0);
        if(field==2) coin.height++;
        if(field==3) coin.isCoinbase=true;
        if(field==4) coin.is_confidential=true;
        if(field==5) coin.commitment={2};
        Rejected([&]{(void)prepare(wrong);});
    }
    auto duplicate=changes;duplicate.push_back(changes[0]);Rejected([&]{(void)prepare(duplicate);});
    auto unexpected=changes;unexpected[1].outpoint=untouched;Rejected([&]{(void)prepare(unexpected);});
    auto missing=changes;missing[0].outpoint=added;Rejected([&]{(void)prepare(missing);});
    Rejected([&]{(void)PreparedUTXOPublication::PrepareUnderLock(live,1,H(10),H(90),changes,after,2,H(11),after_root);});
    Rejected([&]{(void)PreparedUTXOPublication::PrepareUnderLock(live,1,H(10),before_root,changes,after,3,H(11),after_root);});
    Rejected([&]{(void)PreparedUTXOPublication::PrepareUnderLock(live,1,H(10),before_root,changes,after,2,H(11),H(90));});
    bool allocation_failed=false;
    refuse_allocation=true;
    try{(void)prepare(changes);}catch(const std::bad_alloc&){allocation_failed=true;}
    refuse_allocation=false;
    CHECK(allocation_failed && live.HaveCoin(spent) && !live.HaveCoin(added));
    auto prepared=prepare(changes);
    live.SetBestBlock(H(90),1);Rejected([&]{prepared.CheckReadyUnderLock();});
    live.SetBestBlock(H(10),1);prepared.CheckReadyUnderLock();
    auto changed=a;changed.value=AmountUna::Una(99);
    live.DeleteCoin(spent);CHECK(live.AddCoin(spent,changed));
    Rejected([&]{prepared.CheckReadyUnderLock();});
    live.DeleteCoin(spent);CHECK(live.AddCoin(spent,a));
    prepared.CheckReadyUnderLock();
    auto moved=std::move(prepared);
    Rejected([&]{prepared.CheckReadyUnderLock();});
    // The isolated test stands in for the outer durable commit. The real
    // database/Orchard connect+disconnect test uses the same publication API.
    refuse_allocation=true;
    std::move(moved).PublishAfterCommitUnderLock();
    refuse_allocation=false;
    CHECK(!live.HaveCoin(spent) && live.HaveCoin(added));
    CHECK(live.GetCoin(added)->value==b.value && live.GetCoin(untouched)->commitment==ct.commitment);
    CHECK(live.GetHeight()==2 && live.GetBestBlock()==H(11));
    CHECK(live.SnapshotForestCommitment()==after.getCommitment());
    const std::vector<UTXOPublicationChange> undo{{spent,{},a},{added,b,{}}};
    auto back=PreparedUTXOPublication::PrepareUnderLock(live,2,H(11),after_root,undo,before,1,H(10),before_root);
    back.CheckReadyUnderLock();
    refuse_allocation=true;std::move(back).PublishAfterCommitUnderLock();refuse_allocation=false;
    CHECK(live.HaveCoin(spent) && !live.HaveCoin(added) && live.HaveCoin(untouched));
    CHECK(live.GetHeight()==1 && live.GetBestBlock()==H(10));
    CHECK(live.SnapshotForestCommitment()==before.getCommitment());
    std::cout<<"PASS: prepared memory connect/disconnect, abandonment, exact coin/context checks and allocation-free publication\n";
}
